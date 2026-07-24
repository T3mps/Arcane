#include <Arcane/Render/Batcher2D.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
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

        constexpr uint16_t kBuiltInMaterialCount = 3;   // sprite / circle / text

        // One recorded draw (a quad: 4 vertices already in m_vertices).
        // End() stable-sorts records by key, then builds index data and
        // batch runs in sorted order. This is the v1 "compile" -- batcher
        // v2 compiles the same records into per-instance data instead.
        struct DrawRecord
        {
            uint64_t key = 0;
            uint32_t firstVertex = 0;
            uint16_t material = 0;   // Batcher2D::kMaterialSprite..
            nvrhi::ITexture* texture = nullptr;
        };

        // One contiguous run of sorted records sharing material + texture.
        struct BatchRun
        {
            uint16_t material = 0;
            nvrhi::ITexture* texture = nullptr;
            uint32_t firstIndex = 0;
            uint32_t indexCount = 0;
        };

        // Binding sets cache on (texture, material group). Built-ins all share
        // group 0 (identical layout + set contents), so pre-material content
        // still gets exactly one set per texture.
        struct BindKey
        {
            nvrhi::ITexture* texture = nullptr;
            uint16_t materialGroup = 0;
            bool operator==(const BindKey&) const = default;
        };
        struct BindKeyHash
        {
            size_t operator()(const BindKey& k) const noexcept
            {
                return std::hash<void*>{}(k.texture) * 31 + k.materialGroup;
            }
        };

        struct PipeKey
        {
            size_t fbHash = 0;
            uint16_t material = 0;
            bool operator==(const PipeKey&) const = default;
        };
        struct PipeKeyHash
        {
            size_t operator()(const PipeKey& k) const noexcept
            {
                return k.fbHash * 31 + k.material;
            }
        };

        // One material-table entry. Built-ins (0..2) resolve shaders through
        // the ShaderLibrary by name every pipeline build (hot reload via
        // Generation); registered entries own their compiled handles + the
        // layout/values pair and a dedicated binding layout / volatile CB.
        struct MaterialEntry
        {
            bool builtIn = true;
            const char* vsName = "sprite_vs";
            const char* psName = "sprite_ps";
            Material2DDesc desc;                       // registered entries only
            nvrhi::BindingLayoutHandle layout;         // registered entries only
            nvrhi::BufferHandle materialCb;            // when templ->CbSize() > 0
            std::vector<uint8_t> packBuffer;
            bool packedThisBatch = false;              // transient End() flag
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
                    !m_shaders.Get("circle_ps", nvrhi::ShaderType::Pixel) ||
                    !m_shaders.Get("msdf_vs", nvrhi::ShaderType::Vertex) ||
                    !m_shaders.Get("msdf_ps", nvrhi::ShaderType::Pixel))
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

                // Built-in material-table entries 0..2 -- the pre-material
                // pipelines, so the materialless path is the degenerate case.
                m_materials.resize(kBuiltInMaterialCount);
                m_materials[kMaterialSprite] = MaterialEntry{ true, "sprite_vs", "sprite_ps" };
                m_materials[kMaterialCircle] = MaterialEntry{ true, "circle_vs", "circle_ps" };
                m_materials[kMaterialText]   = MaterialEntry{ true, "msdf_vs",   "msdf_ps" };
                return true;
            }

            uint16_t RegisterMaterial(Material2DDesc desc) override
            {
                MaterialEntry entry;
                if (!BuildEntry(entry, std::move(desc)))
                    return kInvalidMaterialId;
                m_materials.push_back(std::move(entry));
                return static_cast<uint16_t>(m_materials.size() - 1);
            }

            bool UpdateMaterial(uint16_t id, Material2DDesc desc) override
            {
                if (id < kBuiltInMaterialCount || id >= m_materials.size())
                {
                    ARC_WARN("Batcher2D::UpdateMaterial: invalid id {}", id);
                    return false;
                }
                MaterialEntry entry;
                if (!BuildEntry(entry, std::move(desc)))
                    return false;
                m_materials[id] = std::move(entry);
                // The slot's pipelines/binding sets referenced the OLD shaders
                // and buffers -- drop exactly them.
                for (auto it = m_pipelines.begin(); it != m_pipelines.end();)
                    it = it->first.material == id ? m_pipelines.erase(it) : std::next(it);
                for (auto it = m_bindingSets.begin(); it != m_bindingSets.end();)
                    it = it->first.materialGroup == id ? m_bindingSets.erase(it) : std::next(it);
                return true;
            }

            void SetGlobals(const GlobalParams& globals) override
            {
                m_globals = globals;
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
                      glm::vec4 color, float rotation) override
            {
                PushQuad(kMaterialSprite, texture ? texture : m_whiteTexture.Get(),
                         dstPos, dstSize, uvMin, uvMax, color, rotation);
            }

            void QuadMaterial(uint16_t materialId, glm::vec2 dstPos, glm::vec2 dstSize,
                              nvrhi::ITexture* texture, glm::vec2 uvMin, glm::vec2 uvMax,
                              glm::vec4 color, float rotation) override
            {
                if (materialId >= m_materials.size())
                    materialId = kMaterialSprite;   // unknown id -> plain sprite
                PushQuad(materialId, texture ? texture : m_whiteTexture.Get(),
                         dstPos, dstSize, uvMin, uvMax, color, rotation);
            }

            void Glyph(glm::vec2 dstPos, glm::vec2 dstSize,
                       nvrhi::ITexture* atlas, glm::vec2 uvMin,
                       glm::vec2 uvMax, glm::vec4 color) override
            {
                PushQuad(kMaterialText, atlas ? atlas : m_whiteTexture.Get(),
                         dstPos, dstSize, uvMin, uvMax, color);
            }

            void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color,
                      float rotation) override
            {
                PushQuad(kMaterialSprite, m_whiteTexture.Get(),
                         pos, size, glm::vec2(0), glm::vec2(1), color, rotation);
            }

            void Line(glm::vec2 a, glm::vec2 b, float thickness,
                      glm::vec4 color) override
            {
                const glm::vec2 delta = b - a;
                const float len = glm::length(delta);
                if (len <= 0.0f || thickness <= 0.0f)
                    return;
                const glm::vec2 normal =
                    glm::vec2(-delta.y, delta.x) * (0.5f * thickness / len);
                // Oriented quad through the shared record path; reuses the
                // sprite pipeline with the white texture (uv constant).
                const glm::vec2 uv(0.5f);
                PushQuadVertices(kMaterialSprite, m_whiteTexture.Get(),
                                 { a - normal, uv, color },
                                 { b - normal, uv, color },
                                 { b + normal, uv, color },
                                 { a + normal, uv, color });
            }

            void Circle(glm::vec2 center, float radius, glm::vec4 color) override
            {
                if (radius <= 0.0f)
                    return;
                // SDF quad: uv spans [-1,1]; circle.hlsl keeps the unit disc.
                PushQuad(kMaterialCircle, m_whiteTexture.Get(),
                         center - glm::vec2(radius), glm::vec2(radius * 2.0f),
                         glm::vec2(-1.0f), glm::vec2(1.0f), color);
            }

            void Triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c,
                          glm::vec4 color) override
            {
                // Solid tri via the shared record path: a quad with v3 == v2, so
                // the second sub-triangle (v0,v2,v3) is degenerate and draws
                // nothing. White texture + constant uv on the sprite pipeline --
                // the same solid-fill path Line/Rect use.
                const glm::vec2 uv(0.5f);
                PushQuadVertices(kMaterialSprite, m_whiteTexture.Get(),
                                 { a, uv, color },
                                 { b, uv, color },
                                 { c, uv, color },
                                 { c, uv, color });
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
                bool anyRegistered = false;
                for (const DrawRecord& record : m_records)
                {
                    if (m_runs.empty() || m_runs.back().material != record.material ||
                        m_runs.back().texture != record.texture)
                    {
                        BatchRun run;
                        run.material = record.material;
                        run.texture = record.texture;
                        run.firstIndex = (uint32_t)m_indices.size();
                        m_runs.push_back(run);
                    }
                    anyRegistered = anyRegistered ||
                                    record.material >= kBuiltInMaterialCount;
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

                // Registered-material uploads: volatile CBs must be written in
                // THIS command list before use. One globals write + one pack
                // per material used this batch.
                if (anyRegistered)
                {
                    if (m_globalsCb)
                        m_commandList->writeBuffer(m_globalsCb, &m_globals,
                                                   sizeof(m_globals));
                    for (const BatchRun& run : m_runs)
                    {
                        if (run.material < kBuiltInMaterialCount)
                            continue;
                        MaterialEntry& e = m_materials[run.material];
                        if (!e.materialCb || e.packedThisBatch)
                            continue;
                        e.desc.instance->PackCB(e.packBuffer.data(), e.packBuffer.size());
                        m_commandList->writeBuffer(e.materialCb, e.packBuffer.data(),
                                                   e.packBuffer.size());
                        e.packedThisBatch = true;
                    }
                    for (MaterialEntry& e : m_materials)
                        e.packedThisBatch = false;
                }

                const PushConstants push{
                    glm::vec2(2.0f / m_viewport.x, 2.0f / m_viewport.y),
                    glm::vec2(0.0f) };

                for (const BatchRun& run : m_runs)
                {
                    nvrhi::IGraphicsPipeline* pipeline = GetPipeline(run.material);
                    if (!pipeline)
                        continue;
                    auto state = nvrhi::GraphicsState()
                        .setPipeline(pipeline)
                        .setFramebuffer(m_target)
                        .addBindingSet(GetBindingSet(run.texture, run.material))
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

            void RemoveTexture(nvrhi::ITexture* texture) override
            {
                // Evict-before-release (mirrors ImGuiNvrhiRenderer::
                // DestroyTexture): dropping the entries releases the cached
                // sets' references so the texture can actually free, and a
                // later texture reusing this address gets a FRESH set from
                // GetBindingSet instead of a stale one (ABA). The per-Begin
                // slot maps (m_textureSlots/m_textureSlotLookup) never outlive
                // a frame, so this is the only cross-frame texture state.
                // One entry may exist per (texture, material group).
                for (auto it = m_bindingSets.begin(); it != m_bindingSets.end();)
                    it = it->first.texture == texture ? m_bindingSets.erase(it)
                                                      : std::next(it);
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

            void PushQuadVertices(uint16_t material, nvrhi::ITexture* texture,
                                  const Vertex& v0, const Vertex& v1,
                                  const Vertex& v2, const Vertex& v3)
            {
                // Key layout: layer(16) | order(16) | material(16) | slot(16).
                // Built-ins keep their old kind values as material 0..2, so
                // materialless content sorts (and draws) exactly as before.
                DrawRecord record;
                record.key = ((uint64_t)m_layer << 48) |
                             ((uint64_t)m_order << 32) |
                             ((uint64_t)material << 16) |
                             (uint64_t)TextureSlot(texture);
                record.firstVertex = (uint32_t)m_vertices.size();
                record.material = material;
                record.texture = texture;
                m_records.push_back(record);
                m_vertices.push_back(v0);
                m_vertices.push_back(v1);
                m_vertices.push_back(v2);
                m_vertices.push_back(v3);
            }

            void PushQuad(uint16_t material, nvrhi::ITexture* texture,
                          glm::vec2 pos, glm::vec2 size,
                          glm::vec2 uvMin, glm::vec2 uvMax, glm::vec4 color,
                          float rotation = 0.0f)
            {
                // Corners in TL,TR,BR,BL order; rotation 0 is the axis-aligned
                // (byte-identical) path. UVs map to the corner order unchanged.
                const std::array<glm::vec2, 4> p = QuadCorners(pos, size, rotation);
                PushQuadVertices(material, texture,
                    { p[0], uvMin, color },
                    { p[1], { uvMax.x, uvMin.y }, color },
                    { p[2], uvMax, color },
                    { p[3], { uvMin.x, uvMax.y }, color });
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

            // Build a registered entry's GPU objects into `out` (create-into-
            // locals: a failure leaves the table untouched).
            bool BuildEntry(MaterialEntry& out, Material2DDesc desc)
            {
                if (!desc.vs || !desc.ps || !desc.templ || !desc.instance)
                {
                    ARC_WARN("Batcher2D::RegisterMaterial: null shader/template/instance");
                    return false;
                }
                if (desc.paramTextures.size() != desc.templ->TextureCount())
                    desc.paramTextures.resize(desc.templ->TextureCount());

                // Layout mirrors sprite_material.hlsl's register map: push
                // constants b0, material CB b1 (when numeric params exist),
                // globals CB b2, sprite texture t0, declared textures t1..,
                // sampler s0.
                auto layoutDesc = nvrhi::BindingLayoutDesc()
                    .setVisibility(nvrhi::ShaderType::All)
                    .addItem(nvrhi::BindingLayoutItem::PushConstants(
                        0, sizeof(PushConstants)));
                if (desc.templ->CbSize() > 0)
                    layoutDesc.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(
                        kSpriteMaterialCbSlot));
                layoutDesc.addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(
                    kSpriteGlobalCbSlot));
                layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
                for (uint32_t t = 0; t < desc.templ->TextureCount(); ++t)
                    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(
                        kSpriteMaterialTextureBase + t));
                layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));

                MaterialEntry entry;
                entry.builtIn = false;
                entry.layout = m_device->createBindingLayout(layoutDesc);
                if (desc.templ->CbSize() > 0)
                {
                    entry.materialCb = m_device->createBuffer(
                        nvrhi::BufferDesc()
                            .setByteSize(desc.templ->CbSize())
                            .setIsConstantBuffer(true)
                            .setIsVolatile(true)
                            .setMaxVersions(16)
                            .setDebugName("Batcher2D.MaterialCB"));
                }
                if (!m_globalsCb)
                {
                    m_globalsCb = m_device->createBuffer(
                        nvrhi::BufferDesc()
                            .setByteSize(sizeof(GlobalParams))
                            .setIsConstantBuffer(true)
                            .setIsVolatile(true)
                            .setMaxVersions(16)
                            .setDebugName("Batcher2D.GlobalsCB"));
                }
                if (!entry.layout || !m_globalsCb ||
                    (desc.templ->CbSize() > 0 && !entry.materialCb))
                {
                    ARC_WARN("Batcher2D::RegisterMaterial: resource creation failed");
                    return false;
                }
                entry.packBuffer.assign(desc.templ->CbSize(), 0);
                entry.desc = std::move(desc);
                out = std::move(entry);
                return true;
            }

            nvrhi::IBindingSet* GetBindingSet(nvrhi::ITexture* texture,
                                              uint16_t material)
            {
                // Built-ins share one layout AND one set shape, so they all
                // cache under group 0 -- one set per texture, exactly the
                // pre-material behavior. Registered materials add their CBs +
                // param textures, so they cache per (texture, material).
                const bool builtIn = material < kBuiltInMaterialCount;
                const BindKey key{ texture,
                                   builtIn ? (uint16_t)0 : material };
                nvrhi::BindingSetHandle& set = m_bindingSets[key];
                if (!set)
                {
                    auto setDesc = nvrhi::BindingSetDesc()
                        .addItem(nvrhi::BindingSetItem::PushConstants(
                            0, sizeof(PushConstants)));
                    if (builtIn)
                    {
                        setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture))
                               .addItem(nvrhi::BindingSetItem::Sampler(0, m_sampler));
                        set = m_device->createBindingSet(setDesc, m_bindingLayout);
                    }
                    else
                    {
                        const MaterialEntry& e = m_materials[material];
                        if (e.materialCb)
                            setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
                                kSpriteMaterialCbSlot, e.materialCb));
                        setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
                            kSpriteGlobalCbSlot, m_globalsCb));
                        setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture));
                        for (uint32_t t = 0; t < e.desc.templ->TextureCount(); ++t)
                        {
                            nvrhi::ITexture* param = e.desc.paramTextures[t]
                                                         ? e.desc.paramTextures[t].Get()
                                                         : m_whiteTexture.Get();
                            setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                                kSpriteMaterialTextureBase + t, param));
                        }
                        setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, m_sampler));
                        set = m_device->createBindingSet(setDesc, e.layout);
                    }
                }
                return set;
            }

            nvrhi::IGraphicsPipeline* GetPipeline(uint16_t material)
            {
                if (m_pipelineGeneration != m_shaders.Generation())
                {
                    // A ShaderLibrary reload only invalidates BUILT-IN shader
                    // handles, but a full clear is cheap and repopulates lazily.
                    m_pipelines.clear();
                    m_pipelineGeneration = m_shaders.Generation();
                }
                const PipeKey key{
                    std::hash<nvrhi::FramebufferInfo>{}(m_target->getFramebufferInfo()),
                    material };
                nvrhi::GraphicsPipelineHandle& pipeline = m_pipelines[key];
                if (!pipeline)
                {
                    const MaterialEntry& e = m_materials[material];
                    nvrhi::ShaderHandle vs = e.builtIn
                        ? m_shaders.Get(e.vsName, nvrhi::ShaderType::Vertex)
                        : e.desc.vs;
                    nvrhi::ShaderHandle ps = e.builtIn
                        ? m_shaders.Get(e.psName, nvrhi::ShaderType::Pixel)
                        : e.desc.ps;
                    if (!vs || !ps)
                        return nullptr;

                    // All materials share blend/raster/depth (straight alpha).
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
                        .addBindingLayout(e.builtIn ? m_bindingLayout : e.layout);
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
            // Keyed by (texture, material group); each cached set pins its
            // texture alive. RemoveTexture(ITexture*) is the eviction hook --
            // whoever releases a texture the batcher drew with must call it
            // FIRST (see its interface comment in Batcher2D.hpp).
            std::unordered_map<BindKey, nvrhi::BindingSetHandle, BindKeyHash> m_bindingSets;
            std::unordered_map<PipeKey, nvrhi::GraphicsPipelineHandle, PipeKeyHash> m_pipelines;
            uint64_t m_pipelineGeneration = 0;

            // The material table: 0..2 built-ins, registered entries after.
            std::vector<MaterialEntry> m_materials;
            nvrhi::BufferHandle m_globalsCb;   // b2, shared by registered materials
            GlobalParams m_globals{};          // sticky, host-set per frame

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
