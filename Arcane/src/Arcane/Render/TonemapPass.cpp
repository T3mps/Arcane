#include <Arcane/Render/TonemapPass.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <functional>
#include <unordered_map>

namespace Arcane
{
    namespace
    {
        class TonemapPassImpl final : public TonemapPass
        {
        public:
            TonemapPassImpl(nvrhi::IDevice* device, ShaderLibrary& shaders)
                : m_device(device), m_shaders(shaders)
            {
            }

            bool Init()
            {
                auto samplerDesc = nvrhi::SamplerDesc()
                    .setAllFilters(false)   // point: 1:1 canvas -> target
                    .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
                m_sampler = m_device->createSampler(samplerDesc);

                auto layoutDesc = nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::Pixel)
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                    .addItem(nvrhi::BindingLayoutItem::Sampler(0));
                m_bindingLayout = m_device->createBindingLayout(layoutDesc);

                return m_sampler != nullptr && m_bindingLayout != nullptr &&
                       m_shaders.Get("tonemap_vs", nvrhi::ShaderType::Vertex) != nullptr &&
                       m_shaders.Get("tonemap_ps", nvrhi::ShaderType::Pixel)  != nullptr;
            }

            void Run(nvrhi::ICommandList* commandList, nvrhi::ITexture* source,
                     nvrhi::IFramebuffer* target) override
            {
                nvrhi::IGraphicsPipeline* pipeline = GetPipeline(target);
                if (!pipeline)
                    return;

                nvrhi::BindingSetHandle& bindingSet = m_bindingSets[source];
                if (!bindingSet)
                {
                    bindingSet = m_device->createBindingSet(
                        nvrhi::BindingSetDesc()
                            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, source))
                            .addItem(nvrhi::BindingSetItem::Sampler(0, m_sampler)),
                        m_bindingLayout);
                }

                // FramebufferInfoEx carries width/height; use its getViewport().
                const nvrhi::FramebufferInfoEx& fbInfo = target->getFramebufferInfo();
                auto state = nvrhi::GraphicsState()
                    .setPipeline(pipeline)
                    .setFramebuffer(target)
                    .addBindingSet(bindingSet);
                state.viewport.addViewportAndScissorRect(fbInfo.getViewport());
                commandList->setGraphicsState(state);
                commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
            }

        private:
            nvrhi::IGraphicsPipeline* GetPipeline(nvrhi::IFramebuffer* target)
            {
                // Lazy rebuild on hot reload: a generation bump invalidates
                // the whole cache (cheap -- one pipeline per target format).
                if (m_pipelineGeneration != m_shaders.Generation())
                {
                    m_pipelines.clear();
                    m_pipelineGeneration = m_shaders.Generation();
                }

                // Pipeline compatibility = attachment formats + sample count.
                // FramebufferInfo has no getHash(); use std::hash which covers
                // colorFormats + depthFormat + sampleCount + sampleQuality.
                const nvrhi::FramebufferInfoEx& fbInfo = target->getFramebufferInfo();
                const size_t key = std::hash<nvrhi::FramebufferInfo>{}(fbInfo);

                nvrhi::GraphicsPipelineHandle& pipeline = m_pipelines[key];
                if (!pipeline)
                {
                    nvrhi::ShaderHandle vs =
                        m_shaders.Get("tonemap_vs", nvrhi::ShaderType::Vertex);
                    nvrhi::ShaderHandle ps =
                        m_shaders.Get("tonemap_ps", nvrhi::ShaderType::Pixel);
                    if (!vs || !ps)
                    {
                        ARC_ERROR("Tonemap shaders unavailable");
                        return nullptr;
                    }
                    auto desc = nvrhi::GraphicsPipelineDesc()
                        .setVertexShader(vs)
                        .setPixelShader(ps)
                        .addBindingLayout(m_bindingLayout);
                    desc.primType = nvrhi::PrimitiveType::TriangleList;
                    desc.renderState.rasterState.setCullNone();
                    desc.renderState.depthStencilState.disableDepthTest();
                    desc.renderState.depthStencilState.disableStencil();
                    // createGraphicsPipeline takes FramebufferInfo const&;
                    // FramebufferInfoEx derives from FramebufferInfo -- implicit slice.
                    pipeline = m_device->createGraphicsPipeline(desc, fbInfo);
                }
                return pipeline;
            }

            nvrhi::IDevice*          m_device;
            ShaderLibrary&           m_shaders;
            nvrhi::SamplerHandle     m_sampler;
            nvrhi::BindingLayoutHandle m_bindingLayout;
            // Pipeline cache: key = hash of FramebufferInfo (format + sample count).
            std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_pipelines;
            // Binding set cache: keyed on raw ITexture* (one canvas in M2a;
            // stale entries leak one set until the pass dies -- revisit when
            // canvases multiply).
            std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_bindingSets;
            uint64_t m_pipelineGeneration = 0;
        };
    }

    std::unique_ptr<TonemapPass> TonemapPass::Create(nvrhi::IDevice* device,
                                                     ShaderLibrary& shaders)
    {
        auto pass = std::make_unique<TonemapPassImpl>(device, shaders);
        if (!pass->Init())
            return nullptr;
        return pass;
    }
}
