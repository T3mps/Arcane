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
        // Binding sets hold strong texture refs; past this the whole cache drops
        // rather than pinning every texture ever bound for the pass's lifetime.
        constexpr std::size_t kMaxCachedBindingSets = 16;

        struct TextureKeyHash
        {
            std::size_t operator()(const std::vector<nvrhi::ITexture*>& k) const noexcept
            {
                std::size_t h = 0;
                for (nvrhi::ITexture* t : k)
                    h = h * 31 + std::hash<void*>{}(t);
                return h;
            }
        };

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
                // 1x1 transparent black: what pass 0 of a chain reads through
                // InputTexture (there is no previous pass).
                texDesc.setDebugName("MaterialBlackFallback");
                m_black = m_device->createTexture(texDesc);
                if (!m_white || !m_black)
                    return false;
                const std::uint32_t whiteTexel = 0xFFFFFFFFu;
                const std::uint32_t blackTexel = 0x00000000u;
                nvrhi::CommandListHandle upload = m_device->createCommandList();
                upload->open();
                upload->writeTexture(m_white, 0, 0, &whiteTexel, 4);
                upload->writeTexture(m_black, 0, 0, &blackTexel, 4);
                upload->close();
                m_device->executeCommandList(upload);
                return true;
            }

            bool SetMaterial(std::shared_ptr<const MaterialTemplate> templ,
                             nvrhi::ShaderHandle vs, nvrhi::ShaderHandle ps,
                             std::uint32_t chainInputs) override
            {
                if (!templ || !vs || !ps)
                {
                    ARC_WARN("FullscreenMaterialPass::SetMaterial: null template/shader");
                    return false;
                }

                // Create every resource into locals first -- a failure anywhere
                // must leave the previous (last-good) material fully bound, per
                // this pass's own contract.
                // Layout mirrors GenerateMaterialBindings exactly: CB b0 only
                // when numeric params exist, globals CB b1 always (the template
                // declares it), one SRV per texture param + sampler s0.
                // All stages: the vertex stage (%{VERTEX_BODY}) reads the same
                // Material/Globals CBs the pixel stage does.
                auto layoutDesc = nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::All);
                if (templ->CbSize() > 0)
                    layoutDesc.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(kMaterialCbSlot));
                layoutDesc.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(kGlobalCbSlot));
                for (std::uint32_t t = 0; t < templ->TextureCount(); ++t)
                    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(t));
                // Chain mode: upstream-input SRVs after the material's own
                // textures, and the sampler exists even when the material
                // declares none (mirrors GenerateMaterialBindings' emission).
                for (std::uint32_t i = 0; i < chainInputs; ++i)
                    layoutDesc.addItem(
                        nvrhi::BindingLayoutItem::Texture_SRV(templ->TextureCount() + i));
                if (templ->TextureCount() > 0 || chainInputs > 0)
                    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
                nvrhi::BindingLayoutHandle bindingLayout = m_device->createBindingLayout(layoutDesc);

                nvrhi::BufferHandle materialCb;
                if (templ->CbSize() > 0)
                {
                    materialCb = m_device->createBuffer(
                        nvrhi::BufferDesc()
                            .setByteSize(templ->CbSize())
                            .setIsConstantBuffer(true)
                            .setIsVolatile(true)
                            .setMaxVersions(16)
                            .setDebugName("MaterialParamsCB"));
                }
                nvrhi::BufferHandle globalsCb = m_globalsCb;
                if (!globalsCb)
                {
                    globalsCb = m_device->createBuffer(
                        nvrhi::BufferDesc()
                            .setByteSize(sizeof(GlobalParams))
                            .setIsConstantBuffer(true)
                            .setIsVolatile(true)
                            .setMaxVersions(16)
                            .setDebugName("MaterialGlobalsCB"));
                }
                if (!bindingLayout || !globalsCb ||
                    (templ->CbSize() > 0 && !materialCb))
                {
                    ARC_WARN("FullscreenMaterialPass::SetMaterial: resource creation "
                             "failed -- previous material kept");
                    return false;
                }

                // Commit: everything exists, swap atomically. New material = new
                // shaders/layout: every cached pipeline and binding set belongs
                // to the previous one.
                m_template = std::move(templ);
                m_vs = vs;
                m_ps = ps;
                m_chainInputs = chainInputs;
                m_bindingLayout = std::move(bindingLayout);
                m_materialCb = std::move(materialCb);
                m_globalsCb = std::move(globalsCb);
                m_pipelines.clear();
                m_bindingSets.clear();
                m_packBuffer.assign(m_template->CbSize(), 0);
                return true;
            }

            bool Ready() const override { return m_template != nullptr; }

            void Render(nvrhi::ICommandList* commandList,
                        nvrhi::IFramebuffer* target,
                        const MaterialInstance& instance,
                        const GlobalParams& globals,
                        Assets* assets,
                        std::span<nvrhi::ITexture* const> chainInputs) override
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

                nvrhi::BindingSetHandle bindingSet =
                    GetBindingSet(instance, assets, chainInputs);
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
                                                  Assets* assets,
                                                  std::span<nvrhi::ITexture* const> chainInputs)
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
                // Chain mode: the input textures ride in the same key vector,
                // so a rewire is just another cached set. Missing/null entries
                // read black.
                for (std::uint32_t i = 0; i < m_chainInputs; ++i)
                {
                    nvrhi::ITexture* in =
                        i < chainInputs.size() && chainInputs[i] ? chainInputs[i]
                                                                 : m_black.Get();
                    textures.push_back(in);
                }

                // Cache keyed on the FULL resolved texture-pointer table (real
                // key equality, not hash-as-identity; the CBs are stable
                // handles, only texture swaps change the set). Bounded: see
                // kMaxCachedBindingSets.
                auto it = m_bindingSets.find(textures);
                if (it == m_bindingSets.end())
                {
                    if (m_bindingSets.size() >= kMaxCachedBindingSets)
                        m_bindingSets.clear();
                    auto setDesc = nvrhi::BindingSetDesc();
                    if (m_materialCb)
                        setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
                            kMaterialCbSlot, m_materialCb));
                    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
                        kGlobalCbSlot, m_globalsCb));
                    for (std::uint32_t t = 0; t < m_template->TextureCount(); ++t)
                        setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(t, textures[t]));
                    for (std::uint32_t i = 0; i < m_chainInputs; ++i)
                        setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                            m_template->TextureCount() + i,
                            textures[m_template->TextureCount() + i]));
                    if (m_template->TextureCount() > 0 || m_chainInputs > 0)
                        setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, m_sampler));
                    nvrhi::BindingSetHandle set =
                        m_device->createBindingSet(setDesc, m_bindingLayout);
                    it = m_bindingSets.emplace(textures, std::move(set)).first;
                }
                return it->second;
            }

            nvrhi::IDevice*            m_device;
            nvrhi::SamplerHandle       m_sampler;
            nvrhi::TextureHandle       m_white;
            nvrhi::TextureHandle       m_black;   // chain pass 0's InputTexture

            std::shared_ptr<const MaterialTemplate> m_template;
            nvrhi::ShaderHandle        m_vs;
            nvrhi::ShaderHandle        m_ps;
            std::uint32_t              m_chainInputs = 0;
            nvrhi::BindingLayoutHandle m_bindingLayout;
            nvrhi::BufferHandle        m_materialCb;   // b0, sized CbSize (null when 0)
            nvrhi::BufferHandle        m_globalsCb;    // b1, one register
            std::vector<std::uint8_t>  m_packBuffer;

            std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_pipelines;
            std::unordered_map<std::vector<nvrhi::ITexture*>, nvrhi::BindingSetHandle,
                               TextureKeyHash> m_bindingSets;
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
