#include <Arcane/Render/Batcher2D.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>

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
            // The image ASSET this quad samples, nil for the untextured
            // primitives -- Batch2DDrawSpan::textureId's source. An
            // `nvrhi::ITexture*` sat beside it until ABI v15; it was null in
            // every record this process ever built (see Batcher2D::Create).
            Guid textureId{};
        };

        // One contiguous run of sorted records sharing material + texture.
        // Also moved to the header (Batch2DDrawSpan) by the Task 8 extraction
        // -- identical members and names, so `run.material` etc. read the same.
        using BatchRun = Batch2DDrawSpan;

        // One material-table entry. Built-ins (0..2) name their shader
        // artifacts by STRING -- the graph path's Batch2DNode loads those bins
        // itself; registered entries own `desc`, the retained shader bytecode
        // + template + instance MaterialDesc(id) hands the graph path. The
        // dedicated binding layout / volatile CB / CPU pack buffer a
        // registered entry used to also own (layout, materialCb, packBuffer,
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
            Batcher2DImpl() = default;

            bool Init()
            {
                // The whole of construction. m_device/m_shaders and the
                // HasDevice() check over them are GONE at ABI v15 (NRI Phase
                // 5a, Task 9.5b-ii) along with Create's parameters: every call
                // site passed (nullptr, nullptr), so the device-carrying
                // construction this used to gate -- the white texel (create +
                // upload), sampler, binding layout, vertex attribute descs /
                // input layout, and the built-in shader fetch/validation --
                // could never run, and Task 9.5b had already deleted it.
                //
                // Kept as an Init() returning bool rather than folded into the
                // constructor because Create's contract is a factory that can
                // in principle fail; nothing fails today.
                InitMaterialTable();
                return true;
            }

            // Built-in material-table entries 0..2 -- the pre-material
            // pipelines, so the materialless path is the degenerate case.
            // They name shader artifacts by STRING and own no GPU object,
            // which is what lets this class have them at all: a drained span's
            // `material` still means the same thing to the recorder.
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
                // The two cache-eviction loops that ran here -- dropping the
                // slot's NVRHI pipelines and binding sets -- are gone with the
                // maps themselves at ABI v15. Their populator (GetPipeline/
                // GetBindingSet) was deleted at Task 9.5b, so both maps had
                // been permanently empty and both loops permanently no-ops.
                // A consumer that rebuilds its own pipelines from
                // MaterialDesc(id) sees the new `desc` on its next read.
                return true;
            }

            void SetGlobals(const GlobalParams& globals) override
            {
                m_globals = globals;
            }

            void Begin(uint32_t viewportWidth, uint32_t viewportHeight) override
            {
                m_viewport = glm::vec2((float)viewportWidth, (float)viewportHeight);
                m_vertices.clear();
                m_indices.clear();
                m_runs.clear();
                m_records.clear();
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
                      glm::vec2 uvMin, glm::vec2 uvMax,
                      glm::vec4 color, float rotation) override
            {
                PushQuad(kMaterialSprite, Guid::Nil(),
                         dstPos, dstSize, uvMin, uvMax, color, rotation);
            }

            void QuadMaterial(uint16_t materialId, glm::vec2 dstPos, glm::vec2 dstSize,
                              glm::vec2 uvMin, glm::vec2 uvMax,
                              glm::vec4 color, float rotation) override
            {
                if (materialId >= m_materials.size())
                    materialId = kMaterialSprite;   // unknown id -> plain sprite
                PushQuad(materialId, Guid::Nil(),
                         dstPos, dstSize, uvMin, uvMax, color, rotation);
            }

            void QuadTextured(uint16_t materialId, const Guid& textureId,
                              glm::vec2 dstPos, glm::vec2 dstSize,
                              glm::vec2 uvMin, glm::vec2 uvMax,
                              glm::vec4 color, float rotation) override
            {
                if (materialId >= m_materials.size())
                    materialId = kMaterialSprite;   // unknown id -> plain sprite
                // Byte-for-byte QuadMaterial's push, plus the identity: same
                // material, same vertices, one field more in the record.
                PushQuad(materialId, textureId,
                         dstPos, dstSize, uvMin, uvMax, color, rotation);
            }

            void Glyph(glm::vec2 dstPos, glm::vec2 dstSize, glm::vec2 uvMin,
                       glm::vec2 uvMax, glm::vec4 color) override
            {
                // A glyph atlas is a RUNTIME texture (SkylinePacker output), not
                // an asset -- there is no Guid that names it, so the span keeps
                // a nil id and the graph path binds its white texel. Text on the
                // graph path is THE remaining texture gap, and it is a font-atlas
                // residency problem, not a cache one. The `nvrhi::ITexture* atlas`
                // parameter that used to come in here was consumed by End(); it
                // never reached a span, and it went with End()'s body.
                PushQuad(kMaterialText, Guid::Nil(),
                         dstPos, dstSize, uvMin, uvMax, color);
            }

            void Rect(glm::vec2 pos, glm::vec2 size, glm::vec4 color,
                      float rotation) override
            {
                PushQuad(kMaterialSprite, Guid::Nil(),
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
                // sprite material untextured (uv constant).
                const glm::vec2 uv(0.5f);
                PushQuadVertices(kMaterialSprite, Guid::Nil(),
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
                PushQuad(kMaterialCircle, Guid::Nil(),
                         center - glm::vec2(radius), glm::vec2(radius * 2.0f),
                         glm::vec2(-1.0f), glm::vec2(1.0f), color);
            }

            void Triangle(glm::vec2 a, glm::vec2 b, glm::vec2 c,
                          glm::vec4 color) override
            {
                // Solid tri via the shared record path: a quad with v3 == v2, so
                // the second sub-triangle (v0,v2,v3) is degenerate and draws
                // nothing. Untextured + constant uv on the sprite material --
                // the same solid-fill path Line/Rect use.
                const glm::vec2 uv(0.5f);
                PushQuadVertices(kMaterialSprite, Guid::Nil(),
                                 { a, uv, color },
                                 { b, uv, color },
                                 { c, uv, color },
                                 { c, uv, color });
            }

            void End() override
            {
                m_stats = {};
                // THE LOUD REFUSAL (NRI Phase 3, Task 2). End() WAS the NVRHI
                // recorder -- every line it used to run below this point
                // created or bound an NVRHI object. Task 9.5b deleted that
                // body once every Batcher2D in the process proved device-less;
                // ABI v15 (Task 9.5b-ii) then removed NVRHI from the class
                // outright, so there is no longer a device to be missing and
                // nothing here left to record. Said ONCE: a host that got here
                // is calling it every frame, and the second thousand copies of
                // the message would bury the first. The batch is left
                // drainable, which is the whole point: the graph path's
                // recorder reads it through Drain().
                if (!m_warnedDevicelessEnd)
                {
                    m_warnedDevicelessEnd = true;
                    ARC_ERROR("Batcher2D::End records nothing -- it was the NVRHI recorder, "
                              "and NVRHI is gone (ABI v15). Read the batch with Drain(); the "
                              "NRI graph path's Batch2DNode is what issues the draws. Further "
                              "occurrences are silent.");
                }
            }

            Batch2DDrained Drain() override
            {
                DrainInternal();

                // HUD PARITY (NRI Phase 2, Task 12). m_stats is what Stats()
                // reports and what BOTH hosts print as "Quads: %u  Draws: %u";
                // End() used to be the only thing that wrote it. The graph
                // path never calls End() -- End() WAS the NVRHI recorder -- so
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
                // shader artifacts by string), so a REGISTERED id is the
                // only case with anything to hand back.
                if (id < kBuiltInMaterialCount || id >= m_materials.size())
                    return nullptr;
                return &m_materials[id].desc;
            }

            // RemoveTexture(nvrhi::ITexture*) is deleted at ABI v15 with the
            // binding-set cache it swept -- see the header. Nothing replaces
            // it: the batcher holds no GPU object to evict, and the graph
            // path's texture residency is NriTextureCache's business.

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
                    // Split on (material, asset id). The `texture` pointer
                    // this also compared until ABI v15 was null in every
                    // record, so it never separated two runs the id did not:
                    // dropping it is not a coalescing change.
                    if (m_runs.empty() || m_runs.back().material != record.material ||
                        m_runs.back().textureId != record.textureId)
                    {
                        BatchRun run;
                        run.material = record.material;
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

            // Slots are per image-asset Guid, in first-use order within one
            // Begin() bracket -- the low 16 bits of the sort key, so quads
            // sampling the same asset sort together and coalesce into one run.
            // Keyed on a (texture object, Guid) PAIR until ABI v15; the
            // pointer half was null for every quad, so the pair's partition
            // and the Guid's are the same partition.
            uint16_t TextureSlot(const Guid& id)
            {
                const auto next = (uint16_t)m_textureSlotLookup.size();
                auto [it, inserted] = m_textureSlotLookup.try_emplace(id, next);
                (void)inserted;
                return it->second;
            }

            void PushQuadVertices(uint16_t material, const Guid& textureId,
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
                             (uint64_t)TextureSlot(textureId);
                record.firstVertex = (uint32_t)m_vertices.size();
                record.material = material;
                record.textureId = textureId;
                m_records.push_back(record);
                m_vertices.push_back(v0);
                m_vertices.push_back(v1);
                m_vertices.push_back(v2);
                m_vertices.push_back(v3);
            }

            void PushQuad(uint16_t material, const Guid& textureId,
                          glm::vec2 pos, glm::vec2 size,
                          glm::vec2 uvMin, glm::vec2 uvMax, glm::vec4 color,
                          float rotation = 0.0f)
            {
                // Corners in TL,TR,BR,BL order; rotation 0 is the axis-aligned
                // (byte-identical) path. UVs map to the corner order unchanged.
                const std::array<glm::vec2, 4> p = QuadCorners(pos, size, rotation);
                PushQuadVertices(material, textureId,
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
                // that used to build here, gated on a HasDevice() check, is
                // deleted -- NRI Phase 5a, Task 9.5b; the class kept no device
                // at all from 9.5b-ii. It was unreachable twice over even
                // before that deletion: no Batcher2D::Create call site
                // anywhere passed a real device, and even hypothetically it
                // would have built objects nothing consumed -- GetPipeline
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
            // 7, and both only ever populated m_bindingSets/m_pipelines. Those
            // two maps outlived them by one task, permanently empty, only
            // because UpdateMaterial and RemoveTexture still swept them; both
            // sweeps and both maps are gone at ABI v15 (Task 9.5b-ii).

            // NO GPU STATE LIVES HERE, and at ABI v15 none can: this class
            // holds no NVRHI type at all. Deleted across Task 9.5b and
            // 9.5b-ii, in the order their readers died --
            //   m_sampler/m_bindingLayout/m_inputLayout/m_vertexBuffer/
            //   m_indexBuffer/m_pipelineGeneration (9.5b: written only by
            //   Init()/EnsureBuffers/GetPipeline, read only by GetBindingSet/
            //   GetPipeline, all deleted together),
            //   m_bindingSets/m_pipelines (9.5b-ii: permanently empty once
            //   their populator went, so UpdateMaterial's and RemoveTexture's
            //   sweeps over them were no-ops),
            //   m_device/m_shaders/m_whiteTexture (9.5b-ii, with Create's
            //   parameters -- every call site passed nulls),
            //   m_commandList/m_target (9.5b-ii, with Begin's parameters --
            //   only End()'s recording body ever read them).

            // The material table: 0..2 built-ins, registered entries after.
            std::vector<MaterialEntry> m_materials;
            // m_globalsCb (the NVRHI globals CB End() used to write into for
            // registered materials) is DELETED with it -- Task 9.5b. m_globals
            // itself stays: SetGlobals/Drain() are both live (Drain() hands
            // its address out as Batch2DDrained::globals for the graph
            // recorder to read).
            GlobalParams m_globals{};          // sticky, host-set per frame

            glm::vec2 m_viewport{ 0.0f };
            std::vector<Vertex> m_vertices;
            std::vector<uint32_t> m_indices;
            std::vector<DrawRecord> m_records;
            std::vector<BatchRun> m_runs;
            // Asset Guid -> slot index, first-use order, cleared every
            // Begin(). The parallel std::vector<nvrhi::ITexture*> that held
            // the slot's texture object is gone at ABI v15: its elements were
            // never read (only its size, as the next index), and every one of
            // them was null.
            std::unordered_map<Guid, uint16_t> m_textureSlotLookup;
            uint16_t m_layer = 0;
            uint16_t m_order = 0;
            // End()'s records-nothing refusal, said once for the whole run.
            bool m_warnedDevicelessEnd = false;
            // m_warnedRegisteredPipeline (GetPipeline's registered-material
            // refusal warn-once flag) is deleted with GetPipeline -- Task 9.5b.
            // DrainInternal's once-per-Begin() guard.
            bool m_drained = false;
            Batch2DStats m_stats;
        };
    }

    std::unique_ptr<Batcher2D> Batcher2D::Create()
    {
        auto batcher = std::make_unique<Batcher2DImpl>();
        if (!batcher->Init())
            return nullptr;
        return batcher;
    }
}
