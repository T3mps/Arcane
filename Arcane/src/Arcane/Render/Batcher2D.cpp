#include <Arcane/Render/Batcher2D.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    namespace
    {
        struct Vertex
        {
            glm::vec2 pos;
            glm::vec2 uv;
            glm::vec4 color;
        };
        static_assert(sizeof(Vertex) == 32, "vertex layout is the wire format");

        struct PushConstants
        {
            glm::vec2 invHalfViewport;
            glm::vec2 pad;
        };

        enum class BatchKind : uint8_t { Sprite, Circle };

        // One recorded draw (a quad: 4 vertices already in m_vertices).
        // End() stable-sorts records by key, then builds index data and
        // batch runs in sorted order. This is the v1 "compile" -- batcher
        // v2 compiles the same records into per-instance data instead.
        struct DrawRecord
        {
            uint64_t key = 0;
            uint32_t firstVertex = 0;
            BatchKind kind = BatchKind::Sprite;
            nvrhi::ITexture* texture = nullptr;
        };

        // One contiguous run of sorted records sharing kind + texture.
        struct BatchRun
        {
            BatchKind kind = BatchKind::Sprite;
            nvrhi::ITexture* texture = nullptr;
            uint32_t firstIndex = 0;
            uint32_t indexCount = 0;
        };

        class Batcher2DImpl final : public Batcher2D
        {
        public:
            Batcher2DImpl(nvrhi::IDevice* device, ShaderLibrary& shaders)
                : m_device(device), m_shaders(shaders)
            {
            }

            bool Init()
            {
                // 1x1 white texture: the untextured path.
                auto whiteDesc = nvrhi::TextureDesc()
                    .setWidth(1).setHeight(1)
                    .setFormat(nvrhi::Format::RGBA8_UNORM)
                    .setInitialState(nvrhi::ResourceStates::ShaderResource)
                    .setKeepInitialState(true)
                    .setDebugName("BatcherWhite");
                m_whiteTexture = m_device->createTexture(whiteDesc);

                auto samplerDesc = nvrhi::SamplerDesc()
                    .setAllFilters(true)  // linear: sprites scale smoothly
                    .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
                m_sampler = m_device->createSampler(samplerDesc);

                auto layoutDesc = nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::All)
                    .addItem(nvrhi::BindingLayoutItem::PushConstants(
                        0, sizeof(PushConstants)))
                    .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
                    .addItem(nvrhi::BindingLayoutItem::Sampler(0));
                m_bindingLayout = m_device->createBindingLayout(layoutDesc);

                const nvrhi::VertexAttributeDesc attributes[] = {
                    nvrhi::VertexAttributeDesc()
                        .setName("POSITION")
                        .setFormat(nvrhi::Format::RG32_FLOAT)
                        .setOffset(offsetof(Vertex, pos))
                        .setElementStride(sizeof(Vertex)),
                    nvrhi::VertexAttributeDesc()
                        .setName("TEXCOORD")
                        .setFormat(nvrhi::Format::RG32_FLOAT)
                        .setOffset(offsetof(Vertex, uv))
                        .setElementStride(sizeof(Vertex)),
                    nvrhi::VertexAttributeDesc()
                        .setName("COLOR")
                        .setFormat(nvrhi::Format::RGBA32_FLOAT)
                        .setOffset(offsetof(Vertex, color))
                        .setElementStride(sizeof(Vertex)),
                };
                nvrhi::ShaderHandle spriteVs =
                    m_shaders.Get("sprite_vs", nvrhi::ShaderType::Vertex);
                if (!spriteVs)
                    return false;
                m_inputLayout = m_device->createInputLayout(
                    attributes, (uint32_t)std::size(attributes), spriteVs);

                if (!m_whiteTexture || !m_sampler || !m_bindingLayout ||
                    !m_inputLayout ||
                    !m_shaders.Get("sprite_ps", nvrhi::ShaderType::Pixel) ||
                    !m_shaders.Get("circle_vs", nvrhi::ShaderType::Vertex) ||
                    !m_shaders.Get("circle_ps", nvrhi::ShaderType::Pixel))
                    return false;

                // Upload the white texel once at creation through a transient
                // command list -- Begin() must not depend on its first
                // caller's list actually being executed.
                {
                    nvrhi::CommandListHandle upload = m_device->createCommandList();
                    upload->open();
                    const uint32_t white = 0xFFFFFFFFu;
                    upload->writeTexture(m_whiteTexture, 0, 0, &white, 4);
                    upload->close();
                    m_device->executeCommandList(upload);
                }
                return true;
            }

            void Begin(nvrhi::ICommandList* commandList,
                       nvrhi::IFramebuffer* target,
                       uint32_t viewportWidth, uint32_t viewportHeight) override
            {
                m_commandList = commandList;
                m_target = target;
                m_viewport = glm::vec2((float)viewportWidth, (float)viewportHeight);
                m_vertices.clear();
                m_indices.clear();
                m_runs.clear();
                m_records.clear();
                m_textureSlots.clear();
                m_textureSlotLookup.clear();
                m_layer = 0;
                m_order = 0;
            }

            void SetLayer(uint16_t layer, uint16_t orderInLayer) override
            {
                m_layer = layer;
                m_order = orderInLayer;
            }

            void Quad(glm::vec2 dstPos, glm::vec2 dstSize,
                      nvrhi::ITexture* texture, glm::vec2 uvMin, glm::vec2 uvMax,
                      glm::vec4 color) override
            {
                PushQuad(BatchKind::Sprite, texture ? texture : m_whiteTexture.Get(),
                         dstPos, dstSize, uvMin, uvMax, color);
            }

            void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color) override
            {
                PushQuad(BatchKind::Sprite, m_whiteTexture.Get(),
                         pos, size, glm::vec2(0), glm::vec2(1), color);
            }

            void Line(glm::vec2 a, glm::vec2 b, float thickness,
                      glm::vec4 color) override
            {
                ARC_ERROR("Batcher2D::Line lands with the primitives task");
            }

            void Circle(glm::vec2 center, float radius, glm::vec4 color) override
            {
                ARC_ERROR("Batcher2D::Circle lands with the primitives task");
            }

            void End() override
            {
                m_stats = {};
                if (m_records.empty() || !m_commandList)
                {
                    m_commandList = nullptr;
                    return;
                }

                // The sort-key pass: correct transparency ordering (layer,
                // order) AND minimal state changes (kind, texture) in one
                // sort. stable_sort keeps submission order on identical keys.
                std::stable_sort(m_records.begin(), m_records.end(),
                                 [](const DrawRecord& a, const DrawRecord& b)
                                 { return a.key < b.key; });

                m_indices.reserve(m_records.size() * 6);
                for (const DrawRecord& record : m_records)
                {
                    if (m_runs.empty() || m_runs.back().kind != record.kind ||
                        m_runs.back().texture != record.texture)
                    {
                        BatchRun run;
                        run.kind = record.kind;
                        run.texture = record.texture;
                        run.firstIndex = (uint32_t)m_indices.size();
                        m_runs.push_back(run);
                    }
                    const uint32_t base = record.firstVertex;
                    const uint32_t quadIndices[6] = { base, base + 1, base + 2,
                                                      base, base + 2, base + 3 };
                    m_indices.insert(m_indices.end(), quadIndices,
                                     quadIndices + 6);
                    m_runs.back().indexCount += 6;
                }

                EnsureBuffers();
                m_commandList->writeBuffer(m_vertexBuffer, m_vertices.data(),
                                           m_vertices.size() * sizeof(Vertex));
                m_commandList->writeBuffer(m_indexBuffer, m_indices.data(),
                                           m_indices.size() * sizeof(uint32_t));

                const PushConstants push{
                    glm::vec2(2.0f / m_viewport.x, 2.0f / m_viewport.y),
                    glm::vec2(0.0f) };

                for (const BatchRun& run : m_runs)
                {
                    nvrhi::IGraphicsPipeline* pipeline = GetPipeline(run.kind);
                    if (!pipeline)
                        continue;
                    auto state = nvrhi::GraphicsState()
                        .setPipeline(pipeline)
                        .setFramebuffer(m_target)
                        .addBindingSet(GetBindingSet(run.texture))
                        .setIndexBuffer({ m_indexBuffer, nvrhi::Format::R32_UINT, 0 })
                        .addVertexBuffer({ m_vertexBuffer, 0, 0 });
                    state.viewport.addViewportAndScissorRect(
                        nvrhi::Viewport(m_viewport.x, m_viewport.y));
                    m_commandList->setGraphicsState(state);
                    m_commandList->setPushConstants(&push, sizeof(push));
                    m_commandList->drawIndexed(nvrhi::DrawArguments()
                        .setVertexCount(run.indexCount)
                        .setStartIndexLocation(run.firstIndex));
                    ++m_stats.drawCalls;
                }
                m_stats.quads = (uint32_t)m_records.size();
                m_commandList = nullptr;
            }

            Batch2DStats Stats() const override { return m_stats; }

        private:
            uint16_t TextureSlot(nvrhi::ITexture* texture)
            {
                auto [it, inserted] = m_textureSlotLookup.try_emplace(
                    texture, (uint16_t)m_textureSlots.size());
                if (inserted)
                    m_textureSlots.push_back(texture);
                return it->second;
            }

            void PushQuadVertices(BatchKind kind, nvrhi::ITexture* texture,
                                  const Vertex& v0, const Vertex& v1,
                                  const Vertex& v2, const Vertex& v3)
            {
                DrawRecord record;
                record.key = ((uint64_t)m_layer << 48) |
                             ((uint64_t)m_order << 32) |
                             ((uint64_t)kind << 24) |
                             (uint64_t)TextureSlot(texture);
                record.firstVertex = (uint32_t)m_vertices.size();
                record.kind = kind;
                record.texture = texture;
                m_records.push_back(record);
                m_vertices.push_back(v0);
                m_vertices.push_back(v1);
                m_vertices.push_back(v2);
                m_vertices.push_back(v3);
            }

            void PushQuad(BatchKind kind, nvrhi::ITexture* texture,
                          glm::vec2 pos, glm::vec2 size,
                          glm::vec2 uvMin, glm::vec2 uvMax, glm::vec4 color)
            {
                PushQuadVertices(kind, texture,
                    { pos, uvMin, color },
                    { { pos.x + size.x, pos.y }, { uvMax.x, uvMin.y }, color },
                    { pos + size, uvMax, color },
                    { { pos.x, pos.y + size.y }, { uvMin.x, uvMax.y }, color });
            }

            void EnsureBuffers()
            {
                const size_t vertexBytes = m_vertices.size() * sizeof(Vertex);
                const size_t indexBytes = m_indices.size() * sizeof(uint32_t);
                if (!m_vertexBuffer ||
                    m_vertexBuffer->getDesc().byteSize < vertexBytes)
                {
                    m_vertexBuffer = m_device->createBuffer(nvrhi::BufferDesc()
                        .setByteSize(std::max<size_t>(vertexBytes, 64 * 1024))
                        .setIsVertexBuffer(true)
                        .setInitialState(nvrhi::ResourceStates::VertexBuffer)
                        .setKeepInitialState(true)
                        .setDebugName("Batcher2D.VB"));
                }
                if (!m_indexBuffer ||
                    m_indexBuffer->getDesc().byteSize < indexBytes)
                {
                    m_indexBuffer = m_device->createBuffer(nvrhi::BufferDesc()
                        .setByteSize(std::max<size_t>(indexBytes, 32 * 1024))
                        .setIsIndexBuffer(true)
                        .setInitialState(nvrhi::ResourceStates::IndexBuffer)
                        .setKeepInitialState(true)
                        .setDebugName("Batcher2D.IB"));
                }
            }

            nvrhi::IBindingSet* GetBindingSet(nvrhi::ITexture* texture)
            {
                nvrhi::BindingSetHandle& set = m_bindingSets[texture];
                if (!set)
                {
                    set = m_device->createBindingSet(
                        nvrhi::BindingSetDesc()
                            .addItem(nvrhi::BindingSetItem::PushConstants(
                                0, sizeof(PushConstants)))
                            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture))
                            .addItem(nvrhi::BindingSetItem::Sampler(0, m_sampler)),
                        m_bindingLayout);
                }
                return set;
            }

            nvrhi::IGraphicsPipeline* GetPipeline(BatchKind kind)
            {
                if (m_pipelineGeneration != m_shaders.Generation())
                {
                    m_pipelines.clear();
                    m_pipelineGeneration = m_shaders.Generation();
                }
                const size_t key =
                    std::hash<nvrhi::FramebufferInfo>{}(m_target->getFramebufferInfo()) * 2 + (size_t)kind;
                nvrhi::GraphicsPipelineHandle& pipeline = m_pipelines[key];
                if (!pipeline)
                {
                    const bool circle = (kind == BatchKind::Circle);
                    nvrhi::ShaderHandle vs = m_shaders.Get(
                        circle ? "circle_vs" : "sprite_vs",
                        nvrhi::ShaderType::Vertex);
                    nvrhi::ShaderHandle ps = m_shaders.Get(
                        circle ? "circle_ps" : "sprite_ps",
                        nvrhi::ShaderType::Pixel);
                    if (!vs || !ps)
                        return nullptr;

                    nvrhi::BlendState::RenderTarget blend;
                    blend.enableBlend()
                        .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                        .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);

                    auto desc = nvrhi::GraphicsPipelineDesc()
                        .setVertexShader(vs)
                        .setPixelShader(ps)
                        .setInputLayout(m_inputLayout)
                        .addBindingLayout(m_bindingLayout);
                    desc.primType = nvrhi::PrimitiveType::TriangleList;
                    desc.renderState.rasterState.setCullNone();
                    desc.renderState.depthStencilState.disableDepthTest();
                    desc.renderState.depthStencilState.disableStencil();
                    desc.renderState.blendState.setRenderTarget(0, blend);
                    pipeline = m_device->createGraphicsPipeline(
                        desc, m_target->getFramebufferInfo());
                }
                return pipeline;
            }

            nvrhi::IDevice* m_device;
            ShaderLibrary& m_shaders;
            nvrhi::TextureHandle m_whiteTexture;
            nvrhi::SamplerHandle m_sampler;
            nvrhi::BindingLayoutHandle m_bindingLayout;
            nvrhi::InputLayoutHandle m_inputLayout;
            nvrhi::BufferHandle m_vertexBuffer;
            nvrhi::BufferHandle m_indexBuffer;
            // Entries are never evicted: every current texture (white + future
            // atlases) is engine-lifetime. When M2b brings dynamic texture
            // lifetimes, add a RemoveTexture(ITexture*) hook the asset system
            // calls on teardown (cached sets pin their textures alive).
            std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> m_bindingSets;
            std::unordered_map<size_t, nvrhi::GraphicsPipelineHandle> m_pipelines;
            uint64_t m_pipelineGeneration = 0;

            nvrhi::ICommandList* m_commandList = nullptr;
            nvrhi::IFramebuffer* m_target = nullptr;
            glm::vec2 m_viewport{ 0.0f };
            std::vector<Vertex> m_vertices;
            std::vector<uint32_t> m_indices;
            std::vector<DrawRecord> m_records;
            std::vector<BatchRun> m_runs;
            std::vector<nvrhi::ITexture*> m_textureSlots;
            std::unordered_map<nvrhi::ITexture*, uint16_t> m_textureSlotLookup;
            uint16_t m_layer = 0;
            uint16_t m_order = 0;
            Batch2DStats m_stats;
        };
    }

    std::unique_ptr<Batcher2D> Batcher2D::Create(nvrhi::IDevice* device,
                                                 ShaderLibrary& shaders)
    {
        auto batcher = std::make_unique<Batcher2DImpl>(device, shaders);
        if (!batcher->Init())
            return nullptr;
        return batcher;
    }
}
