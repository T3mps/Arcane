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
        // The vertex layout moved to Batcher2D.hpp as Batch2DVertex by the NRI
        // Phase 2 read-interface extraction (Task 8) -- same members, same
        // static_assert, now nameable by the graph path's Batch2DNode. The
        // alias keeps every use site below spelled exactly as it was.
        using Vertex = Batch2DVertex;

        // PushConstants (the per-draw viewport-scale push block End() used to
        // build and bind) is deleted with End()'s recording body -- NRI
        // Phase 5a, Task 9.5b. Nothing else in this file constructed one.

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
        // Generation); registered entries own `desc`, the retained shader
        // bytecode + template + instance MaterialDesc(id) hands the graph
        // path. The dedicated binding layout / volatile CB / CPU pack buffer
        // a registered entry used to also own (layout, materialCb, packBuffer,
        // packedThisBatch) are DELETED outright -- NRI Phase 5a, Task 9.5b --
        // rather than left dead: they existed only to feed GetPipeline/
        // GetBindingSet/End()'s registered-material upload, all deleted with
        // them, and GetPipeline had already refused every registered material
        // since Task 7 regardless.
        struct MaterialEntry
        {
            bool builtIn = true;
            const char* vsName = "sprite_vs";
            const char* psName = "sprite_ps";
            Material2DDesc desc;                       // registered entries only
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
            //
            // NO LONGER CALLED from within this file (NRI Phase 5a, Task
            // 9.5b): Init()/End()/BuildEntry()'s device-carrying branches,
            // its only three call sites, are deleted along with the rest of
            // the dead recorder. Kept, not deleted, deliberately: m_device/
            // m_shaders and the constructor that stores them are the
            // fenced-off surface (Batcher2D.cpp:8's ShaderLibrary.hpp include
            // and the constructor's (nvrhi::IDevice*, ShaderLibrary*)
            // signature -- 9.5b-ii's, because Create's matching signature is
            // an ABI break), and this is a correct, cheap, truthful check on
            // them, not itself unreachable code -- just currently unread.
            [[nodiscard]] bool HasDevice() const noexcept
            {
                return m_device != nullptr && m_shaders != nullptr;
            }

            bool Init()
            {
                // THE SEVERANCE. Device-less: skip every GPU creation and
                // stand the material table up anyway, so Begin/Quad*/Drain --
                // the frame's whole DATA SUPPLY -- run exactly as they do with
                // a device.
                //
                // UNCONDITIONAL now (NRI Phase 5a, Task 9.5b): the
                // device-carrying construction this used to gate behind
                // `if (!HasDevice())` -- the white texel (create + upload),
                // sampler, binding layout, vertex attribute descs / input
                // layout, and the built-in shader fetch/validation -- is
                // deleted outright, not ported. HasDevice() was always false
                // for every instance this process constructs (see End()'s
                // comment for the grep). m_whiteTexture (still read live by
                // Quad/QuadMaterial/QuadTextured/Glyph/Rect/Line/Triangle as
                // the untextured-path fallback) simply stays the null handle
                // it was already always left as.
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
                // and buffers -- drop exactly them. (Both maps are always
                // empty now -- see their declaration's comment, NRI Phase 5a,
                // Task 9.5b -- so this is currently a no-op; left in place
                // because it is still correct, and still exactly what a
                // future populator would need.)
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
                // recorder -- every line it used to run below this point
                // created or bound an NVRHI object -- so a device-less
                // batcher cannot run it. Said ONCE: a host that got here is
                // calling it every frame, and the second thousand copies of
                // the message would bury the first. The batch is left
                // drainable, which is the whole point: the graph path's
                // recorder reads it through Drain().
                //
                // UNCONDITIONAL now (NRI Phase 5a, Task 9.5b): the recording
                // body this used to gate behind `if (!HasDevice())` is
                // deleted outright, not ported -- confirmed dead by grep, not
                // inference: every Batcher2D::Create call tree-wide (2
                // production sites, GpuContext.cpp and
                // ShaderEditorDocument.cpp, and every ArcaneTests site) passes
                // (nullptr, nullptr), so HasDevice() was already always false
                // for every instance this process ever constructs. See the
                // task report for the full grep.
                if (!m_warnedDevicelessEnd)
                {
                    m_warnedDevicelessEnd = true;
                    ARC_ERROR("Batcher2D::End on a DEVICE-LESS batcher -- nothing was recorded. "
                              "End() is the NVRHI recorder; a batcher created with a null device "
                              "is drained (Drain()) by the graph path instead. Further "
                              "occurrences are silent.");
                }
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
                // The one SetGlobals() sets, handed over by address rather
                // than re-fetched, so this frame's consumer reads exactly
                // what was set for it. (End() used to also write this same
                // value into an NVRHI globals CB for registered materials;
                // that write is deleted with the rest of End()'s recording
                // body -- NRI Phase 5a, Task 9.5b -- m_globals itself is
                // unaffected, it was always the graph path's own copy.)
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
                // later texture reusing this address gets a FRESH set rather
                // than a stale one (ABA) from whatever populates m_bindingSets
                // next. The per-Begin slot maps (m_textureSlots/
                // m_textureSlotLookup) never outlive a frame, so this is the
                // only cross-frame texture state. One entry may exist per
                // (texture, material group). (GetBindingSet, the map's only
                // populator, is deleted -- NRI Phase 5a, Task 9.5b -- so this
                // loop currently always runs over an empty map; see
                // m_bindingSets' declaration comment.)
                for (auto it = m_bindingSets.begin(); it != m_bindingSets.end();)
                    it = it->first.texture == texture ? m_bindingSets.erase(it)
                                                      : std::next(it);
            }

            Batch2DStats Stats() const override { return m_stats; }

        private:
            // THE v1 "COMPILE" -- lifted verbatim out of End() by the NRI
            // Phase 2 read-interface extraction (Task 8) so both consumers
            // could run the SAME batching. Nothing here touches a device, a
            // command list or a target; it is pure CPU work over m_records.
            //
            // Drain() (the graph path's Batch2DNode) is its only caller now
            // -- NRI Phase 5a, Task 9.5b deleted End()'s call alongside the
            // rest of its recording body, since End() itself was always
            // unreachable (see End()'s own comment). The idempotency guard
            // (m_drained) stays: it is still correct and still load-bearing
            // for Drain() alone (declaration-time vs render-time re-entry
            // within one Begin() bracket would otherwise append a second
            // copy of every index and every run), it is just no longer
            // shared between two callers.
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
                    // The m_anyRegistered accumulation that used to run here
                    // is deleted -- NRI Phase 5a, Task 9.5b. Its one reader
                    // was End()'s registered-material CB-upload guard, itself
                    // deleted; nothing else ever read the flag.
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

            // EnsureBuffers (the NVRHI vertex/index buffer grow-on-demand this
            // used to sit beside) is deleted with End(), its only caller --
            // NRI Phase 5a, Task 9.5b. m_vertexBuffer/m_indexBuffer go with it
            // below (see the field list at the end of this class).

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

                // The BindingLayoutDesc that used to be built here (mirroring
                // sprite_material.hlsl's register map: push constants b0,
                // material CB b1, globals CB b2, sprite texture t0, declared
                // textures t1.., sampler s0) fed ONLY the deleted
                // createBindingLayout call below -- gone with it.

                MaterialEntry entry;
                entry.builtIn = false;
                // The GPU half (entry.layout, entry.materialCb, m_globalsCb)
                // that used to build here, gated on HasDevice(), is deleted --
                // NRI Phase 5a, Task 9.5b, "the whole NVRHI recorder goes
                // together" deferral this comment used to name is this task.
                // It was unreachable twice over even before this deletion:
                // HasDevice() was always false (no caller anywhere passes a
                // real device, see End()'s comment), and even hypothetically
                // it would have built objects nothing consumed -- GetPipeline
                // (deleted with End(), its only caller) already refused every
                // registered material outright since NRI Phase 5a, Task 7.
                // What survives is `desc` -- the product, since MaterialDesc(id)
                // is how the graph path builds its own pipeline and bindings
                // from the same bytes. (The packBuffer.assign call that used
                // to sit here fed End()'s registered-material CB pack, also
                // deleted.)
                entry.desc = std::move(desc);
                out = std::move(entry);
                return true;
            }

            // GetBindingSet and GetPipeline are deleted -- NRI Phase 5a, Task
            // 9.5b. End() (their only caller) is gutted above; GetPipeline
            // already refused every REGISTERED material outright since Task
            // 7, and both only ever populated m_bindingSets/m_pipelines,
            // which UpdateMaterial/RemoveTexture below still (harmlessly)
            // erase from -- those maps are staying empty now, not a behavior
            // change, since nothing has populated them since Task 7 either.

            // Both NULLABLE since NRI Phase 3, Task 2 -- see HasDevice().
            nvrhi::IDevice* m_device;
            ShaderLibrary* m_shaders;
            nvrhi::TextureHandle m_whiteTexture;
            // m_sampler/m_bindingLayout/m_inputLayout/m_vertexBuffer/
            // m_indexBuffer/m_pipelineGeneration are DELETED -- NRI Phase 5a,
            // Task 9.5b -- their only writers were Init()'s/EnsureBuffers'/
            // GetPipeline's now-deleted device-carrying bodies, and their only
            // readers were GetBindingSet/GetPipeline, deleted with them.
            //
            // m_bindingSets/m_pipelines STAY, even though their only
            // populator (GetBindingSet/GetPipeline) is gone and they are
            // therefore now permanently empty: UpdateMaterial and
            // RemoveTexture below still erase from them, and those two are
            // live, fenced public virtuals this task does not touch. The
            // "each cached set pins its texture alive" behavior these maps
            // used to provide is gone with GetBindingSet -- RemoveTexture's
            // eviction loop is now a no-op over an always-empty map, which is
            // not a behavior change: nothing has populated these maps since
            // NRI Phase 5a, Task 7 either.
            std::unordered_map<BindKey, nvrhi::BindingSetHandle, BindKeyHash> m_bindingSets;
            std::unordered_map<PipeKey, nvrhi::GraphicsPipelineHandle, PipeKeyHash> m_pipelines;

            // The material table: 0..2 built-ins, registered entries after.
            std::vector<MaterialEntry> m_materials;
            // m_globalsCb (the NVRHI globals CB End() used to write into for
            // registered materials) is DELETED with it -- Task 9.5b. m_globals
            // itself stays: SetGlobals/Drain() are both live (Drain() hands
            // its address out as Batch2DDrained::globals for the graph
            // recorder to read).
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
            // m_warnedRegisteredPipeline (GetPipeline's registered-material
            // refusal warn-once flag) is deleted with GetPipeline -- Task 9.5b.
            // DrainInternal's once-per-Begin() guard.
            bool m_drained = false;
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
