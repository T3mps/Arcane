// GpuContext: the ordered engine boot extracted from main.cpp. See the header
// for the teardown contract and for the two flavors (NRI Phase 3, Task 6).
// Create() below is a PURE refactor of the former boot block + the inline
// resize / backbuffer-framebuffer plumbing -- behavior is unchanged; only the
// ownership home moved. CreateForGraph() is its device-less sibling.

#include <Arcane/Host/GpuContext.hpp>

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>

namespace Arcane
{
    namespace
    {
        // ONE window shape for both flavors. The graph flavor deliberately
        // keeps every field the NVRHI flavor uses -- most of all the 1280x720
        // default, because every golden baseline was captured at the NVRHI
        // path's backbuffer size and a different default here would make every
        // stage compare a dimension mismatch rather than a pixel one.
        WindowDesc HostWindowDesc(const HostConfig& cfg)
        {
            WindowDesc wd;
            wd.title  = "Arcane Runtime";
            wd.vulkan = (cfg.backend == GraphicsBackend::Vulkan);
            // Hidden until the first presented frame (Task 8's splash_ready stage
            // calls Show()). A window that exists but has never been drawn is the
            // black rectangle this arc exists to remove. The graph flavor's
            // reveal is the host's too -- RuntimeApp shows it once the graph
            // vehicle (which owns this window's only swapchain) exists.
            wd.hidden = true;
            return wd;
        }
    }

    void GpuContext::AssertNvrhi(const char* accessor) const
    {
        ARC_ASSERT(!m_graphFlavor,
                   "GpuContext: an NVRHI accessor was reached on the GRAPH flavor -- that flavor "
                   "constructs no device/swapchain/shaders/canvas/tonemap/command list. The call "
                   "site needs a GraphFlavor() gate (see GpuContext.hpp).");
        (void)accessor;
    }

    std::unique_ptr<GpuContext> GpuContext::Create(const HostConfig& cfg)
    {
        // Private ctor -> can't use make_unique; the partial unwinds via RAII on any
        // early return (members destruct in reverse declaration order, the shutdown
        // order).
        auto ctx = std::unique_ptr<GpuContext>(new GpuContext());

        // window is the value member that destructs LAST (imgui/inputDevices hold
        // SDL window refs); created FIRST so its lifetime spans every consumer.
        if (!ctx->m_window.Create(HostWindowDesc(cfg))) { ARC_ERROR("GpuContext: window create failed"); return nullptr; }

        RenderDeviceDesc dd;
        dd.backend = cfg.backend;
        ctx->m_device = RenderDevice::Create(dd);
        if (!ctx->m_device) { ARC_ERROR("GpuContext: device create failed"); return nullptr; }

        ctx->m_swapchain = ctx->m_device->CreateSwapchain(ctx->m_window, cfg.vsync);
        if (!ctx->m_swapchain) { ARC_ERROR("GpuContext: swapchain create failed"); return nullptr; }

        ctx->m_shaders = ShaderLibrary::Create(ctx->m_device->Nvrhi(), cfg.backend, "data/shaders");
        if (!ctx->m_shaders) { ARC_ERROR("GpuContext: shader library create failed"); return nullptr; }

        ctx->m_canvas = CreateCanvas(ctx->m_device->Nvrhi(),
                                     ctx->m_swapchain->Width(),
                                     ctx->m_swapchain->Height());
        if (!ctx->m_canvas) { ARC_ERROR("GpuContext: canvas create failed"); return nullptr; }

        ctx->m_batcher = Batcher2D::Create(ctx->m_device->Nvrhi(), ctx->m_shaders.get());
        if (!ctx->m_batcher) { ARC_ERROR("GpuContext: batcher create failed"); return nullptr; }

        ctx->m_tonemap = TonemapPass::Create(ctx->m_device->Nvrhi(), *ctx->m_shaders);
        if (!ctx->m_tonemap) { ARC_ERROR("GpuContext: tonemap create failed"); return nullptr; }

        // ImGuiLayer taps window events -> must outlive nothing but be destroyed
        // before the window; member order (after window) guarantees that.
        ctx->m_imgui = ImGuiLayer::Create(ctx->m_window);
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

        // GPU-progress heartbeat (Task 7). Not fallible from the host's point of
        // view: a device that cannot create event queries yields a no-op counter
        // and the GPU watchdog simply never arms -- losing a diagnostic must
        // never be a reason to fail boot.
        ctx->m_frameProgress = std::make_unique<GpuFrameProgress>(ctx->m_device->Nvrhi());

        return ctx;
    }

    std::unique_ptr<GpuContext> GpuContext::CreateForGraph(const HostConfig& cfg)
    {
        auto ctx = std::unique_ptr<GpuContext>(new GpuContext());
        ctx->m_graphFlavor = true;

        // THE window of the process, created first for the same reason as in
        // Create(): it destructs LAST, and on this path the NRI swapchain the
        // caller builds over it must be gone before it is.
        if (!ctx->m_window.Create(HostWindowDesc(cfg)))
        {
            ARC_ERROR("GpuContext: window create failed (graph flavor)");
            return nullptr;
        }

        // NO device, NO swapchain, NO ShaderLibrary, NO canvas, NO tonemap, NO
        // command list, NO GpuFrameProgress. Each has a graph-side owner:
        // NriGraphContext holds the device + swapchain and reads offline
        // artifacts through its own ShaderBytecode (the same
        // ShaderLibrary::ResolveFlavorDir directory, so ARCANE_SHADER_DIR
        // still moves both paths together); the canvas and the tonemap are
        // graph NODES; the heartbeat is NriDiagnostics::PublishHeartbeat.

        // DEVICE-LESS (NRI Phase 3, Task 2): Begin/SetLayer/Quad*/Drain/
        // RegisterMaterial/SetGlobals/MaterialDesc/Stats are all live -- which
        // is the whole data-supply side of the frame -- and only End(), the
        // NVRHI recorder, refuses. The graph's Batch2DNode DRAINS this
        // instance: one batcher, one batching algorithm, two recorders.
        ctx->m_batcher = Batcher2D::Create(nullptr, nullptr);
        if (!ctx->m_batcher) { ARC_ERROR("GpuContext: batcher create failed (graph flavor)"); return nullptr; }

        // ImGuiLayer has one flavor since NRI Phase 5a, Task 5: context + SDL3
        // platform backend + event tap, no renderer (the graph's ImGuiNriNode
        // is the renderer). Same factory the NVRHI arm above calls.
        ctx->m_imgui = ImGuiLayer::Create(ctx->m_window);
        if (!ctx->m_imgui) { ARC_ERROR("GpuContext: imgui create failed (graph flavor)"); return nullptr; }

        ctx->m_inputDevices = InputDevices::Create();
        if (!ctx->m_inputDevices) { ARC_ERROR("GpuContext: input devices create failed (graph flavor)"); return nullptr; }
        ctx->m_input = InputActions::Create();
        if (!ctx->m_input) { ARC_ERROR("GpuContext: input actions create failed (graph flavor)"); return nullptr; }

        return ctx;
    }

    void GpuContext::OnResize(std::uint32_t w, std::uint32_t h)
    {
        AssertNvrhi("OnResize");
        if (m_graphFlavor)
            return;

        // Cached backbuffer framebuffers reference the old swapchain textures -- drop
        // them, resize the swapchain to the event extent, then resize the canvas to
        // the swapchain's ACTUAL extent (the surface may clamp the requested size).
        m_framebuffers.clear();
        m_swapchain->Resize(w, h);
        m_canvas->Resize(m_swapchain->Width(), m_swapchain->Height());
    }

    Canvas* GpuContext::EnsurePost()
    {
        AssertNvrhi("EnsurePost");
        if (m_graphFlavor)
            return nullptr;

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
        AssertNvrhi("FramebufferFor");
        if (m_graphFlavor)
            return m_nullFramebuffer;

        nvrhi::FramebufferHandle& fb = m_framebuffers[backbuffer];
        if (!fb)
            fb = m_device->Nvrhi()->createFramebuffer(
                nvrhi::FramebufferDesc().addColorAttachment(backbuffer));
        return fb;
    }
}
