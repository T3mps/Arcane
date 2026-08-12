#include <Arcane/Render/OffscreenCanvas.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/FullscreenMaterialChain.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Render/TonemapPass.hpp>

namespace Arcane
{
    namespace
    {
        // Display-referred output. BGRA8_UNORM is exactly the engine
        // backbuffer format (see DeviceD3D12/DeviceVulkan kSwapchainFormat):
        // the tonemap already gamma-2.2 encodes, so a plain UNORM target
        // matches what a real backbuffer shows AND lets the ImGui-NVRHI
        // backend sample it without any extra sRGB conversion. A `_SRGB`
        // format would double-apply gamma on the ImGui sample.
        constexpr nvrhi::Format kOutputFormat = nvrhi::Format::BGRA8_UNORM;

        class OffscreenCanvasImpl final : public OffscreenCanvas
        {
        public:
            explicit OffscreenCanvasImpl(nvrhi::IDevice* device)
                : m_device(device) {}

            bool Init(ShaderLibrary& shaders, uint32_t width, uint32_t height)
            {
                m_batcher = Batcher2D::Create(m_device, shaders);
                if (!m_batcher)
                {
                    ARC_ERROR("OffscreenCanvas: Batcher2D creation failed");
                    return false;
                }
                m_tonemap = TonemapPass::Create(m_device, shaders);
                if (!m_tonemap)
                {
                    ARC_ERROR("OffscreenCanvas: TonemapPass creation failed");
                    return false;
                }
                m_commandList = m_device->createCommandList();
                if (!m_commandList)
                {
                    ARC_ERROR("OffscreenCanvas: command list creation failed");
                    return false;
                }
                return BuildTargets(width, height);
            }

            Batcher2D& Batch() override { return *m_batcher; }

            void Draw(FunctionRef<void(Batcher2D&)> fn,
                      glm::vec4 clear) override
            {
                if (!m_canvas || !m_output)
                    return;

                // Mirror ArcaneRuntime's main render loop: open -> clear canvas ->
                // batcher Begin/draw/End -> tonemap Run -> close -> execute.
                // NVRHI manages all framebuffer transitions; no manual
                // barriers here.
                m_commandList->open();
                // F-8c/F-8e: this ONE site covers the editor's viewport, every
                // open document's preview, and any other offscreen consumer --
                // and it is a submit the editor's own frame file never mentions,
                // because this class owns a private command list. The scope is
                // strictly inside open()/close(): a marker written to a not-open
                // list latches the D3D12 marker layer off permanently and is an
                // access violation on Vulkan.
                {
                    GpuPassScope pass(m_commandList, "pass:canvas");
                    m_commandList->clearTextureFloat(
                        m_canvas->Texture(), nvrhi::AllSubresources,
                        nvrhi::Color(clear.r, clear.g, clear.b, clear.a));

                    m_batcher->Begin(m_commandList, m_canvas->Framebuffer(),
                                     m_width, m_height);
                    if (fn)
                        fn(*m_batcher);
                    m_batcher->End();

                    RunPostAndTonemap();
                }

                m_commandList->close();
                m_device->executeCommandList(m_commandList);
            }

            void DrawPass(FunctionRef<void(nvrhi::ICommandList*,
                                           nvrhi::IFramebuffer*)> fn,
                          glm::vec4 clear) override
            {
                if (!m_canvas || !m_output)
                    return;

                // Same open -> clear -> record -> tonemap -> execute shape as
                // Draw(), with the caller recording a raw pass against the
                // linear canvas framebuffer instead of driving the Batcher2D.
                m_commandList->open();
                // F-8c: named apart from Draw's scope on purpose -- the caller
                // records raw here, so "the GPU died in pass:canvas.raw" points
                // at a different body of code than "in pass:canvas".
                {
                    GpuPassScope pass(m_commandList, "pass:canvas.raw");
                    m_commandList->clearTextureFloat(
                        m_canvas->Texture(), nvrhi::AllSubresources,
                        nvrhi::Color(clear.r, clear.g, clear.b, clear.a));

                    if (fn)
                        fn(m_commandList.Get(), m_canvas->Framebuffer());

                    m_tonemap->Run(m_commandList, m_canvas->Texture(), m_outputFb);
                }

                m_commandList->close();
                m_device->executeCommandList(m_commandList);
            }

            uint64_t TextureId() const override
            {
                // The ImGui-NVRHI backend casts ImTextureID straight back to
                // nvrhi::ITexture* and builds an SRV binding set keyed on it.
                return (uint64_t)(uintptr_t)m_output.Get();
            }

            nvrhi::IFramebuffer* OutputFramebuffer() const override
            {
                return m_outputFb.Get();
            }

            void SetPostChain(FullscreenMaterialChain* chain,
                              const MaterialInstance* instance,
                              Assets* assets) override
            {
                m_postChain    = chain;
                m_postInstance = instance;
                m_postAssets   = assets;
            }

            void SetPostGlobals(const GlobalParams& globals) override
            {
                m_postGlobals = globals;
            }

            void Resize(uint32_t width, uint32_t height) override
            {
                if (width == 0 || height == 0 ||
                    (width == m_width && height == m_height))
                    return;

                // The caller owns frame pacing; tearing down targets the GPU
                // may still read requires an idle (Canvas::Resize does the
                // same).
                m_device->waitForIdle();
                m_outputFb   = nullptr;
                m_output     = nullptr;
                m_canvas     = nullptr;
                m_postCanvas = nullptr;   // lazily rebuilt at the next posted Draw
                m_device->runGarbageCollection();
                BuildTargets(width, height);
            }

            uint32_t Width()  const override { return m_width;  }
            uint32_t Height() const override { return m_height; }

        private:
            // The post hook (slice 2): with a bound chain the scene canvas
            // feeds it as the external "Scene" input, the chain renders into
            // the owned linear post buffer, and the tonemap samples THAT.
            // Without one this is exactly the pre-hook line -- byte-identical.
            void RunPostAndTonemap()
            {
                const bool post = m_postChain && m_postChain->Ready() &&
                                  m_postInstance && EnsurePostTarget();
                if (post)
                {
                    m_postChain->Render(m_commandList, m_postCanvas->Framebuffer(),
                                        *m_postInstance, m_postGlobals,
                                        m_postAssets,
                                        static_cast<std::size_t>(-1),
                                        m_canvas->Texture());
                    m_tonemap->Run(m_commandList, m_postCanvas->Texture(),
                                   m_outputFb);
                }
                else
                {
                    m_tonemap->Run(m_commandList, m_canvas->Texture(), m_outputFb);
                }
            }

            // The post buffer is a second linear Canvas, built lazily so the
            // many chainless OffscreenCanvases (doc previews, thumbnails)
            // never pay for it. Sized with the scene canvas; dropped in
            // Resize().
            bool EnsurePostTarget()
            {
                if (m_postCanvas && m_postCanvas->Width() == m_width &&
                    m_postCanvas->Height() == m_height)
                    return true;
                m_postCanvas = CreateCanvas(m_device, m_width, m_height);
                if (!m_postCanvas)
                    ARC_ERROR("OffscreenCanvas: post buffer creation failed "
                              "({}x{}) -- post chain skipped", m_width, m_height);
                return m_postCanvas != nullptr;
            }

            bool BuildTargets(uint32_t width, uint32_t height)
            {
                // Linear-HDR scene target: the canonical engine 2D draw target.
                m_canvas = CreateCanvas(m_device, width, height);
                if (!m_canvas)
                {
                    ARC_ERROR("OffscreenCanvas: Canvas creation failed ({}x{})",
                              width, height);
                    return false;
                }

                // Display-referred output: render target (tonemap writes it)
                // AND shader resource (ImGui samples it as an SRV).
                // isShaderResource defaults to true in nvrhi::TextureDesc;
                // KeepInitialState lets NVRHI auto-transition between
                // RenderTarget and ShaderResource.
                auto outDesc = nvrhi::TextureDesc()
                    .setWidth(width).setHeight(height)
                    .setFormat(kOutputFormat)
                    .setIsRenderTarget(true)
                    .setInitialState(nvrhi::ResourceStates::ShaderResource)
                    .setKeepInitialState(true)
                    .setDebugName("OffscreenCanvasOutput");
                m_output = m_device->createTexture(outDesc);
                if (!m_output)
                {
                    ARC_ERROR("OffscreenCanvas: output texture creation failed");
                    return false;
                }
                m_outputFb = m_device->createFramebuffer(
                    nvrhi::FramebufferDesc().addColorAttachment(m_output));
                if (!m_outputFb)
                {
                    ARC_ERROR("OffscreenCanvas: output framebuffer creation failed");
                    return false;
                }

                m_width  = width;
                m_height = height;
                return true;
            }

            nvrhi::IDevice*              m_device = nullptr;
            std::unique_ptr<Canvas>      m_canvas;
            std::unique_ptr<Canvas>      m_postCanvas;   // lazy: only chains pay
            std::unique_ptr<Batcher2D>   m_batcher;
            std::unique_ptr<TonemapPass> m_tonemap;

            // Post hook state -- non-owning, sticky (see the header contract).
            FullscreenMaterialChain* m_postChain    = nullptr;
            const MaterialInstance*  m_postInstance = nullptr;
            Assets*                  m_postAssets   = nullptr;
            GlobalParams             m_postGlobals{};
            nvrhi::TextureHandle         m_output;     // display-referred BGRA8 RT + SRV
            nvrhi::FramebufferHandle     m_outputFb;
            nvrhi::CommandListHandle     m_commandList;
            uint32_t                     m_width  = 0;
            uint32_t                     m_height = 0;
        };
    }

    std::unique_ptr<OffscreenCanvas> OffscreenCanvas::Create(
        nvrhi::IDevice* device, ShaderLibrary& shaders,
        uint32_t width, uint32_t height)
    {
        if (!device || width == 0 || height == 0)
        {
            ARC_ERROR("OffscreenCanvas::Create: bad args ({}x{})", width, height);
            return nullptr;
        }

        auto oc = std::make_unique<OffscreenCanvasImpl>(device);
        if (!oc->Init(shaders, width, height))
            return nullptr;
        return oc;
    }
}
