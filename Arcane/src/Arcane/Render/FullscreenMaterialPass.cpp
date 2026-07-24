#include <Arcane/Render/FullscreenMaterialPass.hpp>

#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Project/AssetId.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    namespace
    {
        class FullscreenMaterialPassImpl final : public FullscreenMaterialPass
        {
        public:
            explicit FullscreenMaterialPassImpl(nvrhi::IDevice* device)
                : m_device(device)
            {
            }

            bool Init()
            {
                // Linear-filtered wrap sampler: material textures tile and scale
                // (unlike TonemapPass's point 1:1 blit sampler).
                auto samplerDesc = nvrhi::SamplerDesc()
                    .setAllFilters(true)
                    .setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
                m_sampler = m_device->createSampler(samplerDesc);
                if (!m_sampler)
                    return false;

                // 1x1 white fallback for unset/unresolved texture params (the
                // Batcher2D white-texel pattern).
                auto texDesc = nvrhi::TextureDesc()
                    .setWidth(1).setHeight(1)
                    .setFormat(nvrhi::Format::RGBA8_UNORM)
                    .setInitialState(nvrhi::ResourceStates::ShaderResource)
                    .setKeepInitialState(true)
                    .setDebugName("MaterialWhiteFallback");
                m_white = m_device->createTexture(texDesc);
                if (!m_white)
                    return false;
                const std::uint32_t whiteTexel = 0xFFFFFFFFu;
                nvrhi::CommandListHandle upload = m_device->createCommandList();
                upload->open();
                upload->writeTexture(m_white, 0, 0, &whiteTexel, 4);
                upload->close();
                m_device->executeCommandList(upload);
                return true;
            }

            bool SetMaterial(std::shared_ptr<const MaterialTemplate> templ,
                             nvrhi::ShaderHandle vs, nvrhi::ShaderHandle ps) override
            {
                if (!templ || !vs || !ps)
                {
                    ARC_WARN("FullscreenMaterialPass::SetMaterial: null template/shader");
                    return false;
                }

                m_template = std::move(templ);
                m_vs = vs;
                m_ps = ps;

                // Layout mirrors GenerateMaterialBindings exactly: CB b0 only
                // when numeric params exist, globals CB b1 always (the template
                // declares it), one SRV per texture param + sampler s0.
                auto layoutDesc = nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::Pixel);
                if (m_template->CbSize() > 0)
                    layoutDesc.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(kMaterialCbSlot));
                layoutDesc.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(kGlobalCbSlot));
                for (std::uint32_t t = 0; t < m_template->TextureCount(); ++t)
                    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(t));
                if (m_template->TextureCount() > 0)
                    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
                m_bindingLayout = m_device->createBindingLayout(layoutDesc);

                m_materialCb = nullptr;
                if (m_template->CbSize() > 0)
                {
                    m_materialCb = m_device->createBuffer(
                        nvrhi::BufferDesc()
                            .setByteSize(m_template->CbSize())
                            .setIsConstantBuffer(true)
                            .setIsVolatile(true)
                            .setMaxVersions(16)
                            .setDebugName("MaterialParamsCB"));
                }
                if (!m_globalsCb)
                {
                    m_globalsCb = m_device->createBuffer(
                        nvrhi::BufferDesc()
                            .setByteSize(sizeof(GlobalParams))
                            .setIsConstantBuffer(true)
                            .setIsVolatile(true)
                            .setMaxVersions(16)
                            .setDebugName("MaterialGlobalsCB"));
                }

                // New material = new shaders/layout: every cached pipeline and
                // binding set belongs to the previous one.
                m_pipelines.clear();
                m_bindingSets.clear();
                m_packBuffer.assign(m_template->CbSize(), 0);
                return m_bindingLayout != nullptr && m_globalsCb != nullptr &&
                       (m_template->CbSize() == 0 || m_materialCb != nullptr);
            }

            bool Ready() const override { return m_template != nullptr; }

            void Render(nvrhi::ICommandList* commandList,
                        nvrhi::IFramebuffer* target,
                        const MaterialInstance& instance,
                        const GlobalParams& globals,
                        Assets* assets) override
            {
                if (!Ready() || !commandList || !target)
                    return;
                if (&instance.Template() != m_template.get())
                {
                    ARC_WARN("FullscreenMaterialPass: instance template mismatch -- skipped");
                    return;
                }

                nvrhi::IGraphicsPipeline* pipeline = GetPipeline(target);
                if (!pipeline)
                    return;

                // Param packing + uploads: volatile CBs are written inside the
                // open list (NVRHI versions them per write).
                if (m_materialCb)
                {
                    instance.PackCB(m_packBuffer.data(), m_packBuffer.size());
                    commandList->writeBuffer(m_materialCb, m_packBuffer.data(),
                                             m_packBuffer.size());
                }
                commandList->writeBuffer(m_globalsCb, &globals, sizeof(globals));

                nvrhi::BindingSetHandle bindingSet = GetBindingSet(instance, assets);
                if (!bindingSet)
                    return;

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
                const nvrhi::FramebufferInfoEx& fbInfo = target->getFramebufferInfo();
                const size_t key = std::hash<nvrhi::FramebufferInfo>{}(fbInfo);

                nvrhi::GraphicsPipelineHandle& pipeline = m_pipelines[key];
                if (!pipeline)
                {
                    auto desc = nvrhi::GraphicsPipelineDesc()
                        .setVertexShader(m_vs)
                        .setPixelShader(m_ps)
                        .addBindingLayout(m_bindingLayout);
                    desc.primType = nvrhi::PrimitiveType::TriangleList;
                    desc.renderState.rasterState.setCullNone();
                    desc.renderState.depthStencilState.disableDepthTest();
                    desc.renderState.depthStencilState.disableStencil();
                    pipeline = m_device->createGraphicsPipeline(desc, fbInfo);
                }
                return pipeline;
            }

            nvrhi::BindingSetHandle GetBindingSet(const MaterialInstance& instance,
                                                  Assets* assets)
            {
                // Resolve the instance's texture table (Guid -> GPU handle via
                // the Assets GUID seam; anything unresolved renders white).
                std::vector<nvrhi::ITexture*> textures(m_template->TextureCount(),
                                                       m_white.Get());
                if (m_template->TextureCount() > 0 && assets)
                {
                    const std::vector<Guid> guids = instance.ResolveTextures();
                    for (std::size_t i = 0; i < guids.size(); ++i)
                    {
                        if (!guids[i].IsValid())
                            continue;
                        if (nvrhi::TextureHandle t =
                                assets->GetTexture(AssetId::FromGuid(guids[i])))
                            textures[i] = t.Get();
                    }
                }

                // Cache keyed on the resolved texture pointers (the CBs are
                // stable handles; only texture swaps change the set).
                std::size_t key = 0;
                for (nvrhi::ITexture* t : textures)
                    key = key * 31 + std::hash<void*>{}(t);

                nvrhi::BindingSetHandle& set = m_bindingSets[key];
                if (!set)
                {
                    auto setDesc = nvrhi::BindingSetDesc();
                    if (m_materialCb)
                        setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
                            kMaterialCbSlot, m_materialCb));
                    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
                        kGlobalCbSlot, m_globalsCb));
                    for (std::uint32_t t = 0; t < m_template->TextureCount(); ++t)
                        setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(t, textures[t]));
                    if (m_template->TextureCount() > 0)
                        setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, m_sampler));
                    set = m_device->createBindingSet(setDesc, m_bindingLayout);
                }
                return set;
            }

            nvrhi::IDevice*            m_device;
            nvrhi::SamplerHandle       m_sampler;
            nvrhi::TextureHandle       m_white;

            std::shared_ptr<const MaterialTemplate> m_template;
            nvrhi::ShaderHandle        m_vs;
            nvrhi::ShaderHandle        m_ps;
            nvrhi::BindingLayoutHandle m_bindingLayout;
            nvrhi::BufferHandle        m_materialCb;   // b0, sized CbSize (null when 0)
            nvrhi::BufferHandle        m_globalsCb;    // b1, one register
            std::vector<std::uint8_t>  m_packBuffer;

            std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_pipelines;
            std::unordered_map<size_t, nvrhi::BindingSetHandle> m_bindingSets;
        };
    }

    std::unique_ptr<FullscreenMaterialPass> FullscreenMaterialPass::Create(nvrhi::IDevice* device)
    {
        if (!device)
        {
            ARC_ERROR("FullscreenMaterialPass::Create: null device");
            return nullptr;
        }
        auto pass = std::make_unique<FullscreenMaterialPassImpl>(device);
        if (!pass->Init())
        {
            ARC_ERROR("FullscreenMaterialPass::Create: resource setup failed");
            return nullptr;
        }
        return pass;
    }
}
