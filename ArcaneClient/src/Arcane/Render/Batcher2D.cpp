#include <Arcane/Render/Batcher2D.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    namespace
    {
        // The vertex layout moved to Batcher2D.hpp as Batch2DVertex by the NRI
        // Phase 2 read-interface extraction (Task 8) -- same members, same
        // static_assert, now nameable by the graph path's Batch2DNode. The
        // alias keeps every use site below spelled exactly as it was.
        using Vertex = Batch2DVertex;

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
            // The image ASSET behind `texture`, nil for the untextured
            // primitives -- Batch2DDrawSpan::textureId's source. See that
            // member for why both keys exist.
            Guid textureId{};
        };

        // The per-Begin() texture-slot key: the pair a run is allowed to
        // coalesce over. The pointer alone was enough while every batcher had
        // a device; a DEVICE-LESS one has a null pointer for every sprite, so
        // the asset id is what separates them there.
        struct TextureKey
        {
            nvrhi::ITexture* texture = nullptr;
            Guid id{};
            bool operator==(const TextureKey&) const = default;
        };
        struct TextureKeyHash
        {
            size_t operator()(const TextureKey& k) const noexcept
            {
                return std::hash<void*>{}(k.texture) * 31 + std::hash<Guid>{}(k.id);
            }
        };

        // One contiguous run of sorted records sharing material + texture.
        // Also moved to the header (Batch2DDrawSpan) by the Task 8 extraction
        // -- identical members and names, so `run.material` etc. read the same.
        using BatchRun = Batch2DDrawSpan;

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
        // Generation); registered entries own the layout/values pair, the
        // retained shader bytecode, and a dedicated binding layout / volatile
        // CB. The layout/CB half is DEAD as of NRI Phase 5a, Task 7 -- see
        // GetPipeline, which refuses every registered material now that the
        // compiled shader pair is gone from Material2DDesc.
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
            Batcher2DImpl(nvrhi::IDevice* device, ShaderLibrary* shaders)
                : m_device(device), m_shaders(shaders)
            {
            }

            // The GPU half exists only when BOTH halves were supplied: a
            // device with no ShaderLibrary could build no pipeline, and a
            // ShaderLibrary with no device could create no object. Either
            // missing is therefore ONE state -- device-less -- rather than two
            // half-built ones (NRI Phase 3, Task 2; see Batcher2D::Create).
            [[nodiscard]] bool HasDevice() const noexcept
            {
                return m_device != nullptr && m_shaders != nullptr;
            }

            bool Init()
            {
                // THE SEVERANCE. Device-less: skip every GPU creation below and
                // stand the material table up anyway, so Begin/Quad*/Drain --
                // the frame's whole DATA SUPPLY -- run exactly as they do with
                // a device. Nothing here can fail without a device, which is
                // also why this cannot return null on a missing shader bin: it
                // never looks for one.
                if (!HasDevice())
                {
                    InitMaterialTable();
                    return true;
                }

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
                    m_shaders->Get("sprite_vs", nvrhi::ShaderType::Vertex);
                if (!spriteVs)
                    return false;
                m_inputLayout = m_device->createInputLayout(
                    attributes, (uint32_t)std::size(attributes), spriteVs);

                if (!m_whiteTexture || !m_sampler || !m_bindingLayout ||
                    !m_inputLayout ||
                    !m_shaders->Get("sprite_ps", nvrhi::ShaderType::Pixel) ||
                    !m_shaders->Get("circle_vs", nvrhi::ShaderType::Vertex) ||
                    !m_shaders->Get("circle_ps", nvrhi::ShaderType::Pixel) ||
                    !m_shaders->Get("msdf_vs", nvrhi::ShaderType::Vertex) ||
                    !m_shaders->Get("msdf_ps", nvrhi::ShaderType::Pixel))
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

                InitMaterialTable();
                return true;
            }

            // Built-in material-table entries 0..2 -- the pre-material
            // pipelines, so the materialless path is the degenerate case.
            // They name ShaderLibrary artifacts by STRING and own no GPU
            // object, which is exactly why a device-less batcher can have
            // them: a drained span's `material` still means the same thing.
            void InitMaterialTable()
            {
                m_materials.resize(kBuiltInMaterialCount);
                m_materials[kMaterialSprite] = MaterialEntry{ true, "sprite_vs", "sprite_ps" };
                m_materials[kMaterialCircle] = MaterialEntry{ true, "circle_vs", "circle_ps" };
                m_materials[kMaterialText]   = MaterialEntry{ true, "msdf_vs",   "msdf_ps" };
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
                m_drained = false;
                m_anyRegistered = false;
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
                         Guid::Nil(), dstPos, dstSize, uvMin, uvMax, color, rotation);
            }

            void QuadMaterial(uint16_t materialId, glm::vec2 dstPos, glm::vec2 dstSize,
                              nvrhi::ITexture* texture, glm::vec2 uvMin, glm::vec2 uvMax,
                              glm::vec4 color, float rotation) override
            {
                if (materialId >= m_materials.size())
                    materialId = kMaterialSprite;   // unknown id -> plain sprite
                PushQuad(materialId, texture ? texture : m_whiteTexture.Get(),
                         Guid::Nil(), dstPos, dstSize, uvMin, uvMax, color, rotation);
            }

            void QuadTextured(uint16_t materialId, const Guid& textureId,
                              glm::vec2 dstPos, glm::vec2 dstSize,
                              nvrhi::ITexture* texture, glm::vec2 uvMin, glm::vec2 uvMax,
                              glm::vec4 color, float rotation) override
            {
                if (materialId >= m_materials.size())
                    materialId = kMaterialSprite;   // unknown id -> plain sprite
                // Byte-for-byte QuadMaterial's push, plus the identity. With a
                // device this records exactly what QuadMaterial/Quad would have
                // (same material, same texture object, same vertices) -- the
                // NVRHI floor cannot see the difference.
                PushQuad(materialId, texture ? texture : m_whiteTexture.Get(),
                         textureId, dstPos, dstSize, uvMin, uvMax, color, rotation);
            }

            void Glyph(glm::vec2 dstPos, glm::vec2 dstSize,
                       nvrhi::ITexture* atlas, glm::vec2 uvMin,
                       glm::vec2 uvMax, glm::vec4 color) override
            {
                // A glyph atlas is a RUNTIME texture (SkylinePacker output), not
                // an asset -- there is no Guid that names it, so the span keeps
                // a nil id and the graph path binds its white texel. Text on the
                // graph path is THE remaining texture gap after this task, and
                // it is a font-atlas residency problem, not a cache one.
                PushQuad(kMaterialText, atlas ? atlas : m_whiteTexture.Get(),
                         Guid::Nil(), dstPos, dstSize, uvMin, uvMax, color);
            }

            void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color,
                      float rotation) override
            {
                PushQuad(kMaterialSprite, m_whiteTexture.Get(), Guid::Nil(),
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
                PushQuadVertices(kMaterialSprite, m_whiteTexture.Get(), Guid::Nil(),
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
                PushQuad(kMaterialCircle, m_whiteTexture.Get(), Guid::Nil(),
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
                PushQuadVertices(kMaterialSprite, m_whiteTexture.Get(), Guid::Nil(),
                                 { a, uv, color },
                                 { b, uv, color },
                                 { c, uv, color },
                                 { c, uv, color });
            }

            void End() override
            {
                m_stats = {};
                // THE LOUD REFUSAL (NRI Phase 3, Task 2). End() IS the NVRHI
                // recorder -- every line below it creates or binds an NVRHI
                // object -- so a device-less batcher cannot run it. Said ONCE:
                // a host that got here is calling it every frame, and the
                // second thousand copies of the message would bury the first.
                // The batch is left drainable, which is the whole point: the
                // graph path's recorder reads it through Drain().
                if (!HasDevice())
                {
                    if (!m_warnedDevicelessEnd)
                    {
                        m_warnedDevicelessEnd = true;
                        ARC_ERROR("Batcher2D::End on a DEVICE-LESS batcher -- nothing was recorded. "
                                  "End() is the NVRHI recorder; a batcher created with a null device "
                                  "is drained (Drain()) by the graph path instead. Further "
                                  "occurrences are silent.");
                    }
                    m_commandList = nullptr;
                    return;
                }
                if (m_records.empty() || !m_commandList)
                {
                    m_commandList = nullptr;
                    return;
                }

                DrainInternal();
                const bool anyRegistered = m_anyRegistered;

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

                // Draw-level markers (Task 7): the batcher's per-run flush is the
                // one genuinely draw-granular site in the 2D path -- every other
                // pass in either host records a single draw, where a sub-marker
                // would only restate its enclosing pass scope. Opt-in
                // (`diagnostics.drawMarkers`, default false) and compiled out of
                // Dist; nvrhi markers ONLY, never breadcrumb-ring entries, or a
                // busy frame's runs would evict the pass scopes a crash report is
                // built from.
                for (const BatchRun& run : m_runs)
                {
                    nvrhi::IGraphicsPipeline* pipeline = GetPipeline(run.material);
                    if (!pipeline)
                        continue;
#if !defined(ARCANE_DIST)
                    // Name built only when the toggle is on: this is the inner
                    // loop of every 2D frame, and an snprintf per run for a
                    // string nobody reads is not free.
                    char drawName[48] = "draw:batch";
                    if (GpuDrawMarkersEnabled())
                        std::snprintf(drawName, sizeof(drawName), "draw:batch%lld/mat%u",
                                      static_cast<long long>(&run - m_runs.data()),
                                      run.material);
                    ARC_GPU_DRAW_SCOPE(m_commandList, drawName);
#endif
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

            Batch2DDrained Drain() override
            {
                DrainInternal();

                // HUD PARITY (NRI Phase 2, Task 12). m_stats is what Stats()
                // reports and what BOTH hosts print as "Quads: %u  Draws: %u";
                // End() used to be the only thing that wrote it. The graph
                // path never calls End() -- End() IS the NVRHI recorder -- so
                // the HUD it draws would have read a permanent 0/0 while the
                // NVRHI path read real numbers. That is a TEXT difference in
                // the `full` stage golden, in the one HUD line that exists to
                // say what the frame drew.
                //
                // Written HERE rather than in the graph's node for two
                // reasons: this is the object that owns the numbers, and the
                // TIMING then matches exactly -- both hosts build the HUD
                // BEFORE the render half, so on both paths frame N's HUD
                // reports frame N-1's counts. The counts are End()'s own: one
                // draw call per run, one quad per record.
                //
                // Drain() has exactly one caller (the graph's Batch2DNode), so
                // this cannot perturb the NVRHI path.
                m_stats.drawCalls = (uint32_t)m_runs.size();
                m_stats.quads     = (uint32_t)m_records.size();

                Batch2DDrained out;
                out.vertices = std::span<const Batch2DVertex>(m_vertices);
                out.indices  = std::span<const uint32_t>(m_indices);
                out.spans    = std::span<const Batch2DDrawSpan>(m_runs);
                out.viewport = m_viewport;
                // The same sticky value End() writes into the globals CB for
                // registered materials -- handed over rather than re-fetched by
                // the consumer, so the second recorder cannot bind different
                // globals than the first would have.
                out.globals  = &m_globals;
                return out;
            }

            const Material2DDesc* MaterialDesc(uint16_t id) const override
            {
                // Built-ins carry no Material2DDesc at all (their entries name
                // ShaderLibrary artifacts by string), so a REGISTERED id is the
                // only case with anything to hand back.
                if (id < kBuiltInMaterialCount || id >= m_materials.size())
                    return nullptr;
                return &m_materials[id].desc;
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
            // THE v1 "COMPILE" -- lifted verbatim out of End() by the NRI
            // Phase 2 read-interface extraction (Task 8), so both consumers
            // run the SAME batching: End() records it through NVRHI,
            // Batch2DNode records it through NRI. Nothing here touches a
            // device, a command list or a target; it is pure CPU work over
            // m_records.
            //
            // Idempotent within one Begin() bracket (m_drained): Drain() is
            // called at graph-DECLARATION time on the graph path while End()
            // calls it at record time on the NVRHI path, and re-running it
            // would append a second copy of every index and every run.
            void DrainInternal()
            {
                if (m_drained)
                    return;
                m_drained = true;

                // The sort-key pass: correct transparency ordering (layer,
                // order) AND minimal state changes (kind, texture) in one
                // sort. stable_sort keeps submission order on identical keys.
                std::stable_sort(m_records.begin(), m_records.end(),
                                 [](const DrawRecord& a, const DrawRecord& b)
                                 { return a.key < b.key; });

                m_indices.reserve(m_records.size() * 6);
                for (const DrawRecord& record : m_records)
                {
                    // Split on BOTH keys. With a device the id moves in
                    // lockstep with the pointer (one asset, one texture
                    // object), so this is the same split it always was;
                    // device-less the pointer is null everywhere and the id is
                    // the only thing that separates two sprites' runs.
                    if (m_runs.empty() || m_runs.back().material != record.material ||
                        m_runs.back().texture != record.texture ||
                        m_runs.back().textureId != record.textureId)
                    {
                        BatchRun run;
                        run.material = record.material;
                        run.texture = record.texture;
                        run.textureId = record.textureId;
                        run.firstIndex = (uint32_t)m_indices.size();
                        m_runs.push_back(run);
                    }
                    m_anyRegistered = m_anyRegistered ||
                                      record.material >= kBuiltInMaterialCount;
                    const uint32_t base = record.firstVertex;
                    const uint32_t quadIndices[6] = { base, base + 1, base + 2,
                                                      base, base + 2, base + 3 };
                    m_indices.insert(m_indices.end(), quadIndices,
                                     quadIndices + 6);
                    m_runs.back().indexCount += 6;
                }
            }

            // Slots are per (texture object, image-asset Guid) PAIR. With a
            // device the two are 1:1 -- one asset resolves to one cached
            // texture -- so this partitions exactly as the pointer alone did;
            // device-less every pointer is null and the id does the work.
            // (The one case where the pair splits what the pointer would not:
            // two DISTINCT Guids registered against the same file, which
            // Assets caches as one texture. That costs one extra draw call and
            // no pixels; no project in the tree registers a file twice.)
            uint16_t TextureSlot(nvrhi::ITexture* texture, const Guid& id)
            {
                auto [it, inserted] = m_textureSlotLookup.try_emplace(
                    TextureKey{ texture, id }, (uint16_t)m_textureSlots.size());
                if (inserted)
                    m_textureSlots.push_back(texture);
                return it->second;
            }

            void PushQuadVertices(uint16_t material, nvrhi::ITexture* texture,
                                  const Guid& textureId,
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
                             (uint64_t)TextureSlot(texture, textureId);
                record.firstVertex = (uint32_t)m_vertices.size();
                record.material = material;
                record.texture = texture;
                record.textureId = textureId;
                m_records.push_back(record);
                m_vertices.push_back(v0);
                m_vertices.push_back(v1);
                m_vertices.push_back(v2);
                m_vertices.push_back(v3);
            }

            void PushQuad(uint16_t material, nvrhi::ITexture* texture,
                          const Guid& textureId,
                          glm::vec2 pos, glm::vec2 size,
                          glm::vec2 uvMin, glm::vec2 uvMax, glm::vec4 color,
                          float rotation = 0.0f)
            {
                // Corners in TL,TR,BR,BL order; rotation 0 is the axis-aligned
                // (byte-identical) path. UVs map to the corner order unchanged.
                const std::array<glm::vec2, 4> p = QuadCorners(pos, size, rotation);
                PushQuadVertices(material, texture, textureId,
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
                // THE BLOB RELAXATION (NRI Phase 3, Task 2) CLOSED (NRI Phase
                // 5a, Task 7). It let a registration carry compiled HANDLES or
                // retained BLOBS, because a device-less producer
                // (SpriteMaterialCache with no device) could only make the
                // second. The handles are gone from Material2DDesc, so "either"
                // has collapsed to the one that was always the general case:
                // the BYTES. `templ`/`instance` stay mandatory -- they are the
                // layout and the values, and no recorder can bind without them.
                const bool haveBytes = desc.vsBytes && desc.psBytes
                                    && !desc.vsBytes->empty() && !desc.psBytes->empty();
                if (!haveBytes || !desc.templ || !desc.instance)
                {
                    ARC_WARN("Batcher2D::RegisterMaterial: null template/instance, or no "
                             "retained shader bytecode");
                    return false;
                }

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
                // The GPU half, and ONLY when there is a device to build it
                // on. Device-less these three stay null and are never reached:
                // GetBindingSet/GetPipeline are called from End() alone, which
                // refuses outright above. What survives is `desc` -- which is
                // the product, since MaterialDesc(id) is how the graph path
                // builds its own pipeline and bindings from the same bytes.
                //
                // NEVER REACHED WITH A DEVICE EITHER, since NRI Phase 5a,
                // Task 7: GetPipeline refuses registered materials outright, so
                // nothing consumes `entry.layout`/`materialCb` even when they
                // are built. They are left standing rather than torn out here
                // because the whole NVRHI recorder goes together, and its
                // removal is a vtable change this task is fenced out of.
                if (HasDevice())
                {
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
                        // The declared t1.. slots bind the white texel and
                        // nothing else since NRI Phase 5a, Task 7 deleted
                        // Material2DDesc::paramTextures. UNREACHABLE either
                        // way: GetPipeline refuses every registered material
                        // below, so this arm is never asked for a set. Kept
                        // only so the layout it mirrors stays readable beside
                        // BuildEntry's -- it dies with the recorder.
                        for (uint32_t t = 0; t < e.desc.templ->TextureCount(); ++t)
                            setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                                kSpriteMaterialTextureBase + t, m_whiteTexture.Get()));
                        setDesc.addItem(nvrhi::BindingSetItem::Sampler(0, m_sampler));
                        set = m_device->createBindingSet(setDesc, e.layout);
                    }
                }
                return set;
            }

            nvrhi::IGraphicsPipeline* GetPipeline(uint16_t material)
            {
                if (m_pipelineGeneration != m_shaders->Generation())
                {
                    // A ShaderLibrary reload only invalidates BUILT-IN shader
                    // handles, but a full clear is cheap and repopulates lazily.
                    m_pipelines.clear();
                    m_pipelineGeneration = m_shaders->Generation();
                }
                const PipeKey key{
                    std::hash<nvrhi::FramebufferInfo>{}(m_target->getFramebufferInfo()),
                    material };
                nvrhi::GraphicsPipelineHandle& pipeline = m_pipelines[key];
                if (!pipeline)
                {
                    const MaterialEntry& e = m_materials[material];
                    if (!e.builtIn)
                    {
                        // REGISTERED MATERIALS NO LONGER RENDER HERE (NRI
                        // Phase 5a, Task 7). Their pipelines were built from
                        // Material2DDesc::vs/ps, compiled shader objects that
                        // only the producer holding a device could make; that
                        // pair is deleted, and the retained bytes beside it
                        // belong to the graph recorder, which builds its own
                        // pipelines from them (Batch2DNode).
                        //
                        // NOTHING REACHES THIS. A registered material can only
                        // be drawn here by a batcher that has BOTH a device and
                        // a registration, and no such instance is constructed
                        // anywhere: every production Batcher2D::Create call
                        // passes (nullptr, nullptr), and the only registering
                        // tests are device-less. It refuses loudly rather than
                        // silently skipping the run, because a silent skip is a
                        // missing sprite with no line in the log.
                        if (!m_warnedRegisteredPipeline)
                        {
                            m_warnedRegisteredPipeline = true;
                            ARC_ERROR("Batcher2D: the NVRHI recorder cannot draw REGISTERED "
                                      "material {} -- its compiled shader pair is gone and the "
                                      "retained bytecode is the graph recorder's input. Drain() "
                                      "this batcher instead of End()ing it. Further occurrences "
                                      "are silent.", material);
                        }
                        return nullptr;
                    }
                    nvrhi::ShaderHandle vs =
                        m_shaders->Get(e.vsName, nvrhi::ShaderType::Vertex);
                    nvrhi::ShaderHandle ps =
                        m_shaders->Get(e.psName, nvrhi::ShaderType::Pixel);
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

            // Both NULLABLE since NRI Phase 3, Task 2 -- see HasDevice().
            nvrhi::IDevice* m_device;
            ShaderLibrary* m_shaders;
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
            std::unordered_map<TextureKey, uint16_t, TextureKeyHash> m_textureSlotLookup;
            uint16_t m_layer = 0;
            uint16_t m_order = 0;
            // End()'s device-less refusal, said once for the whole run.
            bool m_warnedDevicelessEnd = false;
            // GetPipeline's registered-material refusal, same once-per-run rule.
            bool m_warnedRegisteredPipeline = false;
            // DrainInternal's once-per-Begin() guard and its one carried
            // result (End() needs it for the registered-material uploads).
            bool m_drained = false;
            bool m_anyRegistered = false;
            Batch2DStats m_stats;
        };
    }

    std::unique_ptr<Batcher2D> Batcher2D::Create(nvrhi::IDevice* device,
                                                 ShaderLibrary* shaders)
    {
        auto batcher = std::make_unique<Batcher2DImpl>(device, shaders);
        if (!batcher->Init())
            return nullptr;
        return batcher;
    }
}
