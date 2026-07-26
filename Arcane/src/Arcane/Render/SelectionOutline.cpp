#include <Arcane/Render/SelectionOutline.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <algorithm>
#include <cstddef>   // offsetof
#include <cstdint>
#include <functional>
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
        // NOTE: the field is exact only within kMaxThicknessPx px of a silhouette; farther pixels
        // stay empty (w==0, discarded by the composite) by design -- JFA passes scale with thickness, not viewport res.

        // BYTE-IDENTICAL to the HLSL `cbuffer SeedCB` in outline_seed.hlsl. HLSL
        // packing: selectedCount(0), int2 cursor(4..12), superSample(12),
        // int2 dim(16..24), uint2 pad(24..32), then the id array (32..288).
        // The ids are declared `uint4 gSelectedIds[16]` in HLSL, NOT `uint[64]`:
        // an array of SCALARS pads every element to its own 16-byte register
        // (1024 bytes), while uint4[16] packs 4 per register and mirrors a tight
        // uint32_t[64] here exactly.
        struct SeedCB
        {
            uint32_t selectedCount;
            int32_t  cursorX, cursorY;
            uint32_t superSample;
            int32_t  dimX, dimY;
            uint32_t pad0, pad1;
            uint32_t selectedIds[64];
        };
        static_assert(sizeof(SeedCB) == 288, "SeedCB must match outline_seed.hlsl SeedCB");
        static_assert(offsetof(SeedCB, selectedIds) == 32, "id array starts at offset 32");

        // BYTE-IDENTICAL to the HLSL `cbuffer JfaCB` in outline_jfa.hlsl. HLSL
        // packing: jump(0), int2 dim(4..12), pad(12) -> 16 bytes.
        struct JfaCB { int32_t jump; int32_t dimX, dimY; int32_t pad; };
        static_assert(sizeof(JfaCB) == 16, "JfaCB must match outline_jfa.hlsl JfaCB");

        // BYTE-IDENTICAL to the HLSL `cbuffer CompositeCB` in outline_composite.hlsl.
        // HLSL packing: gSelectThick(0), gHoverThick(4), gEdgeSoft(8), _pad0(12),
        // int2 gDim(16..24), int2 _pad1(24..32), float4 gSelectColor(32..48),
        // float4 gHoverColor(48..64) -> 64 bytes. glm::vec4 (4-byte aligned) lands
        // at offset 32 naturally, matching the 16-aligned HLSL row.
        struct CompositeCB {
            float selectThick, hoverThick, edgeSoft, pad0;
            int32_t dimX, dimY, pad1a, pad1b;
            glm::vec4 selectColor, hoverColor;
        };
        static_assert(sizeof(CompositeCB) == 64, "CompositeCB must match outline_composite.hlsl");
        static_assert(offsetof(CompositeCB, selectColor) == 32, "selectColor at offset 32");

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
                // composite bindings: t0 = boundary-seeded field (SRV), b0 =
                // CompositeCB. Pixel stage. The straddling band reads distance-to-edge
                // from the field alone -- no seed0 coverage / exterior test needed.
                m_compositeLayout = m_device->createBindingLayout(nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::Pixel)
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                    .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0)));
                if (!m_seedLayout || !m_jfaLayout || !m_compositeLayout)
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
                m_compositeCb = m_device->createBuffer(nvrhi::BufferDesc()
                    .setByteSize(sizeof(CompositeCB))
                    .setIsConstantBuffer(true)
                    .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                    .setKeepInitialState(true)
                    .setDebugName("SelectionOutline.CompositeCB"));
                if (!m_seedCb || !m_jfaCb || !m_compositeCb)
                {
                    ARC_ERROR("SelectionOutline: constant buffer creation failed");
                    return false;
                }

                if (!m_shaders->Get("outline_seed_vs",      nvrhi::ShaderType::Vertex) ||
                    !m_shaders->Get("outline_seed_ps",      nvrhi::ShaderType::Pixel)  ||
                    !m_shaders->Get("outline_jfa_vs",       nvrhi::ShaderType::Vertex) ||
                    !m_shaders->Get("outline_jfa_ps",       nvrhi::ShaderType::Pixel)  ||
                    !m_shaders->Get("outline_composite_vs", nvrhi::ShaderType::Vertex) ||
                    !m_shaders->Get("outline_composite_ps", nvrhi::ShaderType::Pixel))
                {
                    ARC_ERROR("SelectionOutline: shaders unavailable");
                    return false;
                }

                return BuildTargets(w, h);
            }

            void Render(nvrhi::ICommandList* cmd, nvrhi::ITexture* idBuffer,
                        nvrhi::IFramebuffer* target, const Params& p) override
            {
                // seed + JFA build the internal field, then the composite pass
                // blends the two-color outline into `target`. A null `target`
                // still runs seed + JFA (DebugDistanceField consumers / tests).
                if (!cmd || !idBuffer)
                    return;
                if (!m_seed0Fb || !m_pingFb[0] || !m_pingFb[1])
                    return;

                // Lazy rebuild on shader hot reload: a generation bump invalidates
                // every pipeline (seed/jfa are built against the fixed RGBA16_SNORM
                // field format; composite against the external target's format).
                if (m_pipelineGeneration != m_shaders->Generation())
                {
                    m_seedPipeline = nullptr;
                    m_jfaPipeline  = nullptr;
                    m_compositePipelines.clear();
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
                const std::size_t idCount = std::min(p.selectedIds.size(), kMaxSelectedOutlineIds);
                if (p.selectedIds.size() > kMaxSelectedOutlineIds)
                {
                    static bool s_warnedOverflow = false;
                    if (!s_warnedOverflow)
                    {
                        s_warnedOverflow = true;
                        ARC_WARN("SelectionOutline: {} selected ids exceeds the {} the seed CB holds -- "
                                 "outlining the first {}", p.selectedIds.size(),
                                 kMaxSelectedOutlineIds, kMaxSelectedOutlineIds);
                    }
                }
                sc.selectedCount = static_cast<uint32_t>(idCount);
                for (std::size_t i = 0; i < idCount; ++i)
                    sc.selectedIds[i] = p.selectedIds[i];
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
                for (uint32_t i = 0; i <= passes; ++i)
                {
                    JfaCB jc{};
                    jc.jump = (i < passes) ? static_cast<int32_t>(1u << (passes - 1u - i)) : 1;
                    jc.dimX = static_cast<int32_t>(m_width);
                    jc.dimY = static_cast<int32_t>(m_height);
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

                // --- Composite pass: field -> target (display-referred) ---
                // Reads the nearest silhouette EDGE (m_field, t0) and writes the AA
                // amber/cyan outline -- straddling the edge (inside + border + outside)
                // -- straight into the gamma-encoded target (no sRGB convert).
                if (!target)
                    return;   // seed + JFA only (DebugDistanceField path)

                nvrhi::IGraphicsPipeline* compositePipe = GetCompositePipeline(target);
                if (!compositePipe)
                    return;

                CompositeCB cc{};
                cc.selectThick = p.selectThicknessPx;
                cc.hoverThick  = p.hoverThicknessPx;
                cc.edgeSoft    = p.edgeSoftnessPx;
                cc.pad0        = 0.0f;
                cc.dimX = (int32_t)m_width; cc.dimY = (int32_t)m_height;
                cc.pad1a = cc.pad1b = 0;
                cc.selectColor = p.selectColor;
                cc.hoverColor  = p.hoverColor;
                cmd->writeBuffer(m_compositeCb, &cc, sizeof(cc));

                {
                    nvrhi::BindingSetHandle compositeSet = CompositeBindingSet();
                    const nvrhi::FramebufferInfoEx& fbInfo = target->getFramebufferInfo();
                    auto state = nvrhi::GraphicsState()
                        .setPipeline(compositePipe)
                        .setFramebuffer(target)
                        .addBindingSet(compositeSet);
                    state.viewport.addViewportAndScissorRect(fbInfo.getViewport());
                    cmd->setGraphicsState(state);
                    cmd->draw(nvrhi::DrawArguments().setVertexCount(3));
                }
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
                m_compositeBindingSets.clear();
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

            // Composite targets an EXTERNAL framebuffer whose format we do not own,
            // so cache one pipeline per FramebufferInfo (mirrors v1's GetPipeline).
            nvrhi::IGraphicsPipeline* GetCompositePipeline(nvrhi::IFramebuffer* target)
            {
                const nvrhi::FramebufferInfoEx& fbInfo = target->getFramebufferInfo();
                const size_t key = std::hash<nvrhi::FramebufferInfo>{}(fbInfo);

                nvrhi::GraphicsPipelineHandle& pipeline = m_compositePipelines[key];
                if (!pipeline)
                {
                    nvrhi::ShaderHandle vs = m_shaders->Get("outline_composite_vs", nvrhi::ShaderType::Vertex);
                    nvrhi::ShaderHandle ps = m_shaders->Get("outline_composite_ps", nvrhi::ShaderType::Pixel);
                    if (!vs || !ps)
                    {
                        ARC_ERROR("SelectionOutline: outline_composite shaders unavailable");
                        return nullptr;
                    }

                    // Outline texels return a (possibly translucent) color; the rest
                    // discard -- alpha blend so the AA ring composites over the
                    // display-referred target (mirrors v1's blend state).
                    nvrhi::BlendState::RenderTarget blend;
                    blend.enableBlend()
                        .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                        .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);

                    auto desc = nvrhi::GraphicsPipelineDesc()
                        .setVertexShader(vs)
                        .setPixelShader(ps)
                        .addBindingLayout(m_compositeLayout);
                    desc.primType = nvrhi::PrimitiveType::TriangleList;
                    desc.renderState.rasterState.setCullNone();
                    desc.renderState.depthStencilState.disableDepthTest();
                    desc.renderState.depthStencilState.disableStencil();
                    desc.renderState.blendState.setRenderTarget(0, blend);
                    pipeline = m_device->createGraphicsPipeline(desc, fbInfo);
                }
                return pipeline;
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

            nvrhi::BindingSetHandle CompositeBindingSet()
            {
                // t0 = m_field (last-written JFA target; deterministic per size but
                // keyed on the pointer for safety). BuildTargets clears the cache when
                // the owned targets rebuild.
                nvrhi::BindingSetHandle& set = m_compositeBindingSets[m_field];
                if (!set)
                {
                    set = m_device->createBindingSet(
                        nvrhi::BindingSetDesc()
                            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, m_field))
                            .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_compositeCb)),
                        m_compositeLayout);
                }
                return set;
            }

            nvrhi::IDevice*            m_device  = nullptr;
            ShaderLibrary*             m_shaders = nullptr;
            uint32_t                   m_width = 0, m_height = 0;

            nvrhi::BindingLayoutHandle m_seedLayout, m_jfaLayout, m_compositeLayout;
            nvrhi::BufferHandle        m_seedCb, m_jfaCb, m_compositeCb;
            nvrhi::GraphicsPipelineHandle m_seedPipeline, m_jfaPipeline;
            // Composite pipelines are keyed on the external target's FramebufferInfo
            // hash (format may vary) -- mirrors v1's per-target pipeline cache.
            std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_compositePipelines;
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
            // Composite binding set keyed on the m_field pointer. Cleared by
            // BuildTargets on (re)size.
            std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_compositeBindingSets;
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
