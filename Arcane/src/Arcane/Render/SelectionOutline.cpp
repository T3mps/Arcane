#include <Arcane/Render/SelectionOutline.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <cstddef>   // offsetof
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace Arcane
{
    namespace
    {
        // BYTE-IDENTICAL to the HLSL `cbuffer OutlineCB` in selection_outline.hlsl.
        // HLSL constant-buffer packing: gSelectedId(0), int2 gCursorPx(4..12),
        // gSelectThick(12), gHoverThick(16), uint3 _pad(20..32), float4
        // gSelectColor(32), float4 gHoverColor(48) -> 64 bytes. glm::vec4 (4-byte
        // aligned by default) lands at offset 32 naturally, matching the 16-aligned
        // HLSL row.
        struct OutlineCB
        {
            uint32_t  selectedId;
            int32_t   cursorX;
            int32_t   cursorY;
            uint32_t  selectThick;
            uint32_t  hoverThick;
            uint32_t  pad0, pad1, pad2;
            glm::vec4 selectColor;
            glm::vec4 hoverColor;
        };
        static_assert(sizeof(OutlineCB) == 64, "OutlineCB must match the HLSL cbuffer (64 bytes)");
        static_assert(offsetof(OutlineCB, selectColor) == 32, "gSelectColor must land at HLSL offset 32");

        class SelectionOutlineImpl final : public SelectionOutline
        {
        public:
            bool Init(nvrhi::IDevice* device, ShaderLibrary& shaders)
            {
                m_device  = device;
                m_shaders = &shaders;

                // Pixel-stage bindings: t0 = R32_UINT id buffer (SRV), b0 = OutlineCB.
                auto layoutDesc = nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::Pixel)
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                    .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
                m_bindingLayout = m_device->createBindingLayout(layoutDesc);
                if (!m_bindingLayout)
                {
                    ARC_ERROR("SelectionOutline: binding layout creation failed");
                    return false;
                }

                // Non-volatile constant buffer written once per Render via writeBuffer;
                // KeepInitialState lets NVRHI auto-transition CopyDest<->ConstantBuffer.
                m_cb = m_device->createBuffer(nvrhi::BufferDesc()
                    .setByteSize(sizeof(OutlineCB))
                    .setIsConstantBuffer(true)
                    .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                    .setKeepInitialState(true)
                    .setDebugName("SelectionOutline.CB"));
                if (!m_cb)
                {
                    ARC_ERROR("SelectionOutline: constant buffer creation failed");
                    return false;
                }

                if (!m_shaders->Get("selection_outline_vs", nvrhi::ShaderType::Vertex) ||
                    !m_shaders->Get("selection_outline_ps", nvrhi::ShaderType::Pixel))
                {
                    ARC_ERROR("SelectionOutline: shaders unavailable");
                    return false;
                }
                return true;
            }

            void Render(nvrhi::ICommandList* cmd, nvrhi::ITexture* idBuffer,
                        nvrhi::IFramebuffer* target, const Params& p) override
            {
                if (!cmd || !idBuffer || !target)
                    return;

                nvrhi::IGraphicsPipeline* pipeline = GetPipeline(target);
                if (!pipeline)
                    return;

                OutlineCB cb{};
                cb.selectedId  = p.selectedId;
                cb.cursorX     = p.cursorPx.x;
                cb.cursorY     = p.cursorPx.y;
                cb.selectThick = p.selectThicknessPx;
                cb.hoverThick  = p.hoverThicknessPx;
                cb.pad0 = cb.pad1 = cb.pad2 = 0u;
                cb.selectColor = p.selectColor;
                cb.hoverColor  = p.hoverColor;
                cmd->writeBuffer(m_cb, &cb, sizeof(cb));

                // One live id buffer per pass today: evict stale entries so their
                // strong handles stop pinning a destroyed id target's memory
                // (mirrors TonemapPass's single-source binding-set cache).
                if (!m_bindingSets.empty() &&
                    m_bindingSets.find(idBuffer) == m_bindingSets.end())
                    m_bindingSets.clear();

                nvrhi::BindingSetHandle& bindingSet = m_bindingSets[idBuffer];
                if (!bindingSet)
                {
                    bindingSet = m_device->createBindingSet(
                        nvrhi::BindingSetDesc()
                            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, idBuffer))
                            .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_cb)),
                        m_bindingLayout);
                }

                const nvrhi::FramebufferInfoEx& fbInfo = target->getFramebufferInfo();
                auto state = nvrhi::GraphicsState()
                    .setPipeline(pipeline)
                    .setFramebuffer(target)
                    .addBindingSet(bindingSet);
                state.viewport.addViewportAndScissorRect(fbInfo.getViewport());
                cmd->setGraphicsState(state);
                cmd->draw(nvrhi::DrawArguments().setVertexCount(3));
            }

        private:
            nvrhi::IGraphicsPipeline* GetPipeline(nvrhi::IFramebuffer* target)
            {
                // Lazy rebuild on shader hot reload: a generation bump invalidates
                // the whole cache (one pipeline per target format).
                if (m_pipelineGeneration != m_shaders->Generation())
                {
                    m_pipelines.clear();
                    m_pipelineGeneration = m_shaders->Generation();
                }

                const nvrhi::FramebufferInfoEx& fbInfo = target->getFramebufferInfo();
                const size_t key = std::hash<nvrhi::FramebufferInfo>{}(fbInfo);

                nvrhi::GraphicsPipelineHandle& pipeline = m_pipelines[key];
                if (!pipeline)
                {
                    nvrhi::ShaderHandle vs =
                        m_shaders->Get("selection_outline_vs", nvrhi::ShaderType::Vertex);
                    nvrhi::ShaderHandle ps =
                        m_shaders->Get("selection_outline_ps", nvrhi::ShaderType::Pixel);
                    if (!vs || !ps)
                    {
                        ARC_ERROR("SelectionOutline: shaders unavailable");
                        return nullptr;
                    }

                    // Outline pixels return an (opaque) color; the rest discard --
                    // alpha blend so a translucent outline color composites over the
                    // display-referred target (opaque colors overwrite as expected).
                    nvrhi::BlendState::RenderTarget blend;
                    blend.enableBlend()
                        .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                        .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);

                    auto desc = nvrhi::GraphicsPipelineDesc()
                        .setVertexShader(vs)
                        .setPixelShader(ps)
                        .addBindingLayout(m_bindingLayout);
                    desc.primType = nvrhi::PrimitiveType::TriangleList;
                    desc.renderState.rasterState.setCullNone();
                    desc.renderState.depthStencilState.disableDepthTest();
                    desc.renderState.depthStencilState.disableStencil();
                    desc.renderState.blendState.setRenderTarget(0, blend);
                    pipeline = m_device->createGraphicsPipeline(desc, fbInfo);
                }
                return pipeline;
            }

            nvrhi::IDevice*            m_device  = nullptr;
            ShaderLibrary*             m_shaders = nullptr;
            nvrhi::BindingLayoutHandle m_bindingLayout;
            nvrhi::BufferHandle        m_cb;
            // Pipeline cache: key = hash of FramebufferInfo (format + sample count).
            std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_pipelines;
            // Binding-set cache keyed on the raw id ITexture* (one id buffer today;
            // Render() clears the map when a new id-buffer pointer arrives).
            std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_bindingSets;
            uint64_t m_pipelineGeneration = 0;
        };
    }

    std::unique_ptr<SelectionOutline> SelectionOutline::Create(nvrhi::IDevice* device,
                                                              ShaderLibrary& shaders)
    {
        auto pass = std::make_unique<SelectionOutlineImpl>();
        if (!pass->Init(device, shaders))
            return nullptr;
        return pass;
    }
}
