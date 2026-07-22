#include <Arcane/Render/SelectionOutline.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <cstdint>
#include <unordered_map>

namespace Arcane
{
    uint32_t JfaPassCount(uint32_t maxThicknessPx)
    {
        if (maxThicknessPx <= 1u) return 1u;
        uint32_t n = 0; uint32_t v = maxThicknessPx - 1u;
        while (v) { v >>= 1; ++n; }          // n = floor(log2(maxThicknessPx-1))+1 = ceil(log2(maxThicknessPx))
        return n + 1u;
    }

    namespace
    {
        constexpr uint32_t kMaxThicknessPx = 32;   // sizes the JFA pass count (jumps 32..1)

        // BYTE-IDENTICAL to the HLSL `cbuffer SeedCB` in outline_seed.hlsl. HLSL
        // packing: selectedId(0), int2 cursor(4..12), superSample(12), int2 dim(16..24),
        // uint2 pad(24..32) -> 32 bytes.
        struct SeedCB { uint32_t selectedId; int32_t cursorX, cursorY; uint32_t superSample; int32_t dimX, dimY; uint32_t pad0, pad1; };
        static_assert(sizeof(SeedCB) == 32, "SeedCB must match outline_seed.hlsl SeedCB");

        // BYTE-IDENTICAL to the HLSL `cbuffer JfaCB` in outline_jfa.hlsl. HLSL
        // packing: jump(0), int2 dim(4..12), pad(12) -> 16 bytes.
        struct JfaCB { int32_t jump; int32_t dimX, dimY; int32_t pad; };
        static_assert(sizeof(JfaCB) == 16, "JfaCB must match outline_jfa.hlsl JfaCB");

        // Distance-field targets are signed-normalized RGBA16: seed .xy stores a
        // normalized [-1,1] silhouette position, .z a +-1 select/hover tag, .w a
        // 0..1 coverage. Renderable + sample-able on both backends.
        constexpr nvrhi::Format kFieldFormat = nvrhi::Format::RGBA16_SNORM;

        class SelectionOutlineImpl final : public SelectionOutline
        {
        public:
            bool Init(nvrhi::IDevice* device, ShaderLibrary& shaders, uint32_t w, uint32_t h)
            {
                m_device  = device;
                m_shaders = &shaders;

                // seed bindings: t0 = R32_UINT id (SRV), b0 = SeedCB. Pixel stage.
                m_seedLayout = m_device->createBindingLayout(nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::Pixel)
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                    .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0)));
                // jfa bindings: t0 = RGBA16_SNORM seed (SRV), b0 = JfaCB. Pixel stage.
                m_jfaLayout = m_device->createBindingLayout(nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::Pixel)
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                    .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0)));
                if (!m_seedLayout || !m_jfaLayout)
                {
                    ARC_ERROR("SelectionOutline: binding layout creation failed");
                    return false;
                }

                // Non-volatile constant buffers written once per pass via writeBuffer;
                // KeepInitialState lets NVRHI auto-transition CopyDest<->ConstantBuffer.
                m_seedCb = m_device->createBuffer(nvrhi::BufferDesc()
                    .setByteSize(sizeof(SeedCB))
                    .setIsConstantBuffer(true)
                    .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                    .setKeepInitialState(true)
                    .setDebugName("SelectionOutline.SeedCB"));
                m_jfaCb = m_device->createBuffer(nvrhi::BufferDesc()
                    .setByteSize(sizeof(JfaCB))
                    .setIsConstantBuffer(true)
                    .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                    .setKeepInitialState(true)
                    .setDebugName("SelectionOutline.JfaCB"));
                if (!m_seedCb || !m_jfaCb)
                {
                    ARC_ERROR("SelectionOutline: constant buffer creation failed");
                    return false;
                }

                if (!m_shaders->Get("outline_seed_vs", nvrhi::ShaderType::Vertex) ||
                    !m_shaders->Get("outline_seed_ps", nvrhi::ShaderType::Pixel)  ||
                    !m_shaders->Get("outline_jfa_vs",  nvrhi::ShaderType::Vertex) ||
                    !m_shaders->Get("outline_jfa_ps",  nvrhi::ShaderType::Pixel))
                {
                    ARC_ERROR("SelectionOutline: shaders unavailable");
                    return false;
                }

                return BuildTargets(w, h);
            }

            void Render(nvrhi::ICommandList* cmd, nvrhi::ITexture* idBuffer,
                        nvrhi::IFramebuffer* /*target*/, const Params& p) override
            {
                // Task 2: seed + JFA into the internal field. The composite into
                // `target` is Task 3 -- `target` is intentionally unused here.
                if (!cmd || !idBuffer)
                    return;
                if (!m_seed0Fb || !m_pingFb[0] || !m_pingFb[1])
                    return;

                // Lazy rebuild on shader hot reload: a generation bump invalidates
                // both pipelines (built against the fixed RGBA16_SNORM field format).
                if (m_pipelineGeneration != m_shaders->Generation())
                {
                    m_seedPipeline = nullptr;
                    m_jfaPipeline  = nullptr;
                    m_pipelineGeneration = m_shaders->Generation();
                }

                nvrhi::IGraphicsPipeline* seedPipe = GetSeedPipeline();
                nvrhi::IGraphicsPipeline* jfaPipe  = GetJfaPipeline();
                if (!seedPipe || !jfaPipe)
                    return;

                // --- Seed pass: id -> seed0 ---
                const uint32_t idW = idBuffer->getDesc().width;
                const int ss = (m_width > 0) ? (int)(idW / m_width) : 1;

                SeedCB sc{};
                sc.selectedId = p.selectedId;
                sc.cursorX = p.cursorPx.x;
                sc.cursorY = p.cursorPx.y;
                sc.superSample = (uint32_t)ss;
                sc.dimX = (int32_t)m_width;
                sc.dimY = (int32_t)m_height;
                sc.pad0 = sc.pad1 = 0u;
                cmd->writeBuffer(m_seedCb, &sc, sizeof(sc));

                {
                    nvrhi::BindingSetHandle seedSet = SeedBindingSet(idBuffer);
                    const nvrhi::FramebufferInfoEx& fbInfo = m_seed0Fb->getFramebufferInfo();
                    auto state = nvrhi::GraphicsState()
                        .setPipeline(seedPipe)
                        .setFramebuffer(m_seed0Fb)
                        .addBindingSet(seedSet);
                    state.viewport.addViewportAndScissorRect(fbInfo.getViewport());
                    cmd->setGraphicsState(state);
                    cmd->draw(nvrhi::DrawArguments().setVertexCount(3));
                }

                // --- JFA ping-pong: seed0 -> ... -> field ---
                // Each pass reads `src` and writes `dstFb` (always DISTINCT targets),
                // then advances src<-dst and flips dst between the two ping targets.
                // m_field ends pointing at the LAST-WRITTEN target.
                const uint32_t passes = JfaPassCount(kMaxThicknessPx);
                nvrhi::ITexture* src = m_seed0;                 // first read source
                nvrhi::IFramebuffer* dstFb = m_pingFb[0];       // first write
                nvrhi::ITexture* dst = m_ping[0];
                for (uint32_t i = 0; i < passes; ++i)
                {
                    JfaCB jc{};
                    jc.jump = 1 << (passes - 1 - i);
                    jc.dimX = (int32_t)m_width;
                    jc.dimY = (int32_t)m_height;
                    jc.pad  = 0;
                    cmd->writeBuffer(m_jfaCb, &jc, sizeof(jc));

                    nvrhi::BindingSetHandle jfaSet = JfaBindingSet(src);
                    const nvrhi::FramebufferInfoEx& fbInfo = dstFb->getFramebufferInfo();
                    auto state = nvrhi::GraphicsState()
                        .setPipeline(jfaPipe)
                        .setFramebuffer(dstFb)
                        .addBindingSet(jfaSet);
                    state.viewport.addViewportAndScissorRect(fbInfo.getViewport());
                    cmd->setGraphicsState(state);
                    cmd->draw(nvrhi::DrawArguments().setVertexCount(3));

                    src = dst;
                    const uint32_t nextIdx = (i + 1) & 1u;      // ping <-> pong
                    dst = m_ping[nextIdx]; dstFb = m_pingFb[nextIdx];
                }
                m_field = src;   // the last-written target holds the distance field
            }

            void Resize(uint32_t w, uint32_t h) override
            {
                if (w == 0 || h == 0 || (w == m_width && h == m_height))
                    return;

                // The caller owns frame pacing; tearing down targets the GPU may
                // still read requires an idle (PickBuffer/OffscreenCanvas do the same).
                m_device->waitForIdle();
                m_seed0Fb = nullptr; m_pingFb[0] = nullptr; m_pingFb[1] = nullptr;
                m_seed0   = nullptr; m_ping[0]   = nullptr; m_ping[1]   = nullptr;
                m_field   = nullptr;
                m_device->runGarbageCollection();
                BuildTargets(w, h);
            }

            nvrhi::ITexture* DebugDistanceField() const override { return m_field; }

        private:
            bool BuildTargets(uint32_t w, uint32_t h)
            {
                m_width = w; m_height = h;

                auto makeTarget = [&](const char* name) -> nvrhi::TextureHandle {
                    return m_device->createTexture(nvrhi::TextureDesc()
                        .setWidth(w).setHeight(h)
                        .setFormat(kFieldFormat)
                        .setIsRenderTarget(true)   // isShaderResource defaults true (read next pass)
                        .setInitialState(nvrhi::ResourceStates::RenderTarget)
                        .setKeepInitialState(true) // auto-transition RenderTarget<->ShaderResource
                        .setDebugName(name));
                };

                m_seed0   = makeTarget("SelectionOutline.Seed0");
                m_ping[0] = makeTarget("SelectionOutline.PingA");
                m_ping[1] = makeTarget("SelectionOutline.PingB");
                if (!m_seed0 || !m_ping[0] || !m_ping[1])
                {
                    ARC_ERROR("SelectionOutline: field target creation failed ({}x{})", w, h);
                    return false;
                }

                m_seed0Fb   = m_device->createFramebuffer(
                    nvrhi::FramebufferDesc().addColorAttachment(m_seed0));
                m_pingFb[0] = m_device->createFramebuffer(
                    nvrhi::FramebufferDesc().addColorAttachment(m_ping[0]));
                m_pingFb[1] = m_device->createFramebuffer(
                    nvrhi::FramebufferDesc().addColorAttachment(m_ping[1]));
                if (!m_seed0Fb || !m_pingFb[0] || !m_pingFb[1])
                {
                    ARC_ERROR("SelectionOutline: field framebuffer creation failed");
                    return false;
                }

                // Owned targets were (re)created -> any cached binding set now
                // references a destroyed texture; drop them so Render rebuilds.
                m_seedBindingSets.clear();
                m_jfaBindingSets.clear();
                m_field = m_seed0;   // until the first Render populates the field
                return true;
            }

            nvrhi::IGraphicsPipeline* GetSeedPipeline()
            {
                if (!m_seedPipeline)
                {
                    nvrhi::ShaderHandle vs = m_shaders->Get("outline_seed_vs", nvrhi::ShaderType::Vertex);
                    nvrhi::ShaderHandle ps = m_shaders->Get("outline_seed_ps", nvrhi::ShaderType::Pixel);
                    if (!vs || !ps)
                    {
                        ARC_ERROR("SelectionOutline: outline_seed shaders unavailable");
                        return nullptr;
                    }
                    m_seedPipeline = MakeFullscreenPipeline(vs, ps, m_seedLayout, m_seed0Fb);
                }
                return m_seedPipeline;
            }

            nvrhi::IGraphicsPipeline* GetJfaPipeline()
            {
                if (!m_jfaPipeline)
                {
                    nvrhi::ShaderHandle vs = m_shaders->Get("outline_jfa_vs", nvrhi::ShaderType::Vertex);
                    nvrhi::ShaderHandle ps = m_shaders->Get("outline_jfa_ps", nvrhi::ShaderType::Pixel);
                    if (!vs || !ps)
                    {
                        ARC_ERROR("SelectionOutline: outline_jfa shaders unavailable");
                        return nullptr;
                    }
                    // All field framebuffers share one RGBA16_SNORM FramebufferInfo, so a
                    // pipeline built against pingFb[0] is compatible with pingFb[1] too.
                    m_jfaPipeline = MakeFullscreenPipeline(vs, ps, m_jfaLayout, m_pingFb[0]);
                }
                return m_jfaPipeline;
            }

            nvrhi::GraphicsPipelineHandle MakeFullscreenPipeline(
                nvrhi::ShaderHandle vs, nvrhi::ShaderHandle ps,
                nvrhi::IBindingLayout* layout, nvrhi::IFramebuffer* fb)
            {
                auto desc = nvrhi::GraphicsPipelineDesc()
                    .setVertexShader(vs)
                    .setPixelShader(ps)
                    .addBindingLayout(layout);
                desc.primType = nvrhi::PrimitiveType::TriangleList;
                desc.renderState.rasterState.setCullNone();
                desc.renderState.depthStencilState.disableDepthTest();
                desc.renderState.depthStencilState.disableStencil();
                // No blend: seed/jfa fully overwrite their RGBA16_SNORM target.
                return m_device->createGraphicsPipeline(desc, fb->getFramebufferInfo());
            }

            nvrhi::BindingSetHandle SeedBindingSet(nvrhi::ITexture* idBuffer)
            {
                // One id buffer per pass today: evict stale entries so their strong
                // handles stop pinning a destroyed id target (mirrors v1 / TonemapPass).
                if (!m_seedBindingSets.empty() &&
                    m_seedBindingSets.find(idBuffer) == m_seedBindingSets.end())
                    m_seedBindingSets.clear();

                nvrhi::BindingSetHandle& set = m_seedBindingSets[idBuffer];
                if (!set)
                {
                    set = m_device->createBindingSet(
                        nvrhi::BindingSetDesc()
                            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, idBuffer))
                            .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_seedCb)),
                        m_seedLayout);
                }
                return set;
            }

            nvrhi::BindingSetHandle JfaBindingSet(nvrhi::ITexture* src)
            {
                // src cycles over {seed0, pingA, pingB} within one Render -- cache all
                // three; BuildTargets clears the map when the owned targets rebuild.
                nvrhi::BindingSetHandle& set = m_jfaBindingSets[src];
                if (!set)
                {
                    set = m_device->createBindingSet(
                        nvrhi::BindingSetDesc()
                            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, src))
                            .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_jfaCb)),
                        m_jfaLayout);
                }
                return set;
            }

            nvrhi::IDevice*            m_device  = nullptr;
            ShaderLibrary*             m_shaders = nullptr;
            uint32_t                   m_width = 0, m_height = 0;

            nvrhi::BindingLayoutHandle m_seedLayout, m_jfaLayout;
            nvrhi::BufferHandle        m_seedCb, m_jfaCb;
            nvrhi::GraphicsPipelineHandle m_seedPipeline, m_jfaPipeline;
            uint64_t                   m_pipelineGeneration = 0;

            // Owned distance-field targets: seed pass writes m_seed0, JFA ping-pongs
            // between m_ping[0]/m_ping[1]; m_field points at the last-written one.
            nvrhi::TextureHandle       m_seed0, m_ping[2];
            nvrhi::FramebufferHandle   m_seed0Fb, m_pingFb[2];
            nvrhi::ITexture*           m_field = nullptr;

            // Binding-set caches keyed on the raw source ITexture* (seed: id buffer;
            // jfa: seed0/pingA/pingB). Cleared by BuildTargets on (re)size.
            std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_seedBindingSets;
            std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_jfaBindingSets;
        };
    }

    std::unique_ptr<SelectionOutline> SelectionOutline::Create(nvrhi::IDevice* device, ShaderLibrary& shaders,
                                                              uint32_t w, uint32_t h)
    {
        auto pass = std::make_unique<SelectionOutlineImpl>();
        if (!pass->Init(device, shaders, w, h))
            return nullptr;
        return pass;
    }
}
