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

        constexpr uint16_t kBuiltInMaterialCount = 3;   // sprite / circle / text

        // One recorded draw (a quad: 4 vertices already in m_vertices).
        // DrainInternal stable-sorts records by key, then builds index data
        // and batch runs in sorted order. This is the v1 "compile" -- batcher
        // v2 compiles the same records into per-instance data instead.
        struct DrawRecord
        {
            uint64_t key = 0;
            uint32_t firstVertex = 0;
            uint16_t material = 0;   // Batcher2D::kMaterialSprite..
            // The image ASSET this quad samples, nil for the untextured
            // primitives -- Batch2DDrawSpan::textureId's source.
            Guid textureId{};
        };

        // One contiguous run of sorted records sharing material + texture.
        // Also moved to the header (Batch2DDrawSpan) by the Task 8 extraction
        // -- identical members and names, so `run.material` etc. read the same.
        using BatchRun = Batch2DDrawSpan;

        // One material-table entry. Built-ins (0..2) name their shader
        // artifacts by STRING -- Batch2DNode loads those bins itself;
        // registered entries own `desc`, the retained shader bytecode +
        // template + instance that MaterialDesc(id) hands the recorder.
        // An entry owns NO GPU state -- no binding layout, no constant
        // buffer, no pack buffer -- because the recorder builds all of that
        // from `desc` itself.
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
                // The whole of construction, and it is CPU-only: with no
                // device there is no white texel, sampler, binding layout or
                // input layout to create, and no built-in shader to fetch.
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
                // NOTHING TO EVICT: this class caches no pipeline and no
                // binding set. A consumer that rebuilds its own pipelines
                // from MaterialDesc(id) sees the new `desc` on its next
                // read.
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
                // a nil id and the recorder binds its white texel. Text is THE
                // remaining texture gap, and it is a font-atlas residency
                // problem, not a cache one.
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
                // THE LOUD REFUSAL. End() records nothing: this class holds
                // no device and creates no GPU object, so there is nothing
                // here left to record. Said ONCE -- a host that got here is
                // calling it every frame, and the second thousand copies of
                // the message would bury the first. The batch is left
                // drainable, which is the whole point: the recorder reads it
                // through Drain().
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

                // THE HUD'S NUMBERS. m_stats is what Stats() reports and
                // what both hosts print as "Quads: %u  Draws: %u". Written
                // HERE, in Drain(), rather than in the recorder: this is the
                // object that owns the numbers, and nothing else writes them.
                //
                // The TIMING follows from where the hosts build their HUD --
                // both build it BEFORE the render half, so frame N's HUD
                // reports frame N-1's counts. One draw call per run, one quad
                // per record.
                m_stats.drawCalls = (uint32_t)m_runs.size();
                m_stats.quads     = (uint32_t)m_records.size();

                Batch2DDrained out;
                out.vertices = std::span<const Batch2DVertex>(m_vertices);
                out.indices  = std::span<const uint32_t>(m_indices);
                out.spans    = std::span<const Batch2DDrawSpan>(m_runs);
                out.viewport = m_viewport;
                // The one SetGlobals() sets, handed over by address rather
                // than re-fetched, so this frame's consumer reads exactly
                // what was set for it.
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

            // The batcher holds no GPU object to evict; texture residency
            // is NriTextureCache's business -- see the header.

            Batch2DStats Stats() const override { return m_stats; }

        private:
            // THE v1 "COMPILE". Nothing here touches a device, a command
            // list or a target; it is pure CPU work over m_records.
            //
            // Drain() is its only caller. The idempotency guard (m_drained)
            // is load-bearing for it: declaration-time vs render-time
            // re-entry within one Begin() bracket would otherwise append a
            // second copy of every index and every run.
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

            // Build a registered entry into `out` (create-into-locals: a
            // failure leaves the table untouched).
            bool BuildEntry(MaterialEntry& out, Material2DDesc desc)
            {
                // A REGISTRATION IS BYTES -- there is no compiled-handle
                // alternative, because a device-less producer could not make
                // one. `templ`/`instance` are mandatory too: they are the
                // layout and the values, and no recorder can bind without
                // them.
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
                // `desc` IS THE PRODUCT, and the whole of it: there is no
                // GPU half to build here, because MaterialDesc(id) is how the
                // recorder builds its own pipeline and bindings from these
                // same bytes.
                entry.desc = std::move(desc);
                out = std::move(entry);
                return true;
            }

            // NO GPU STATE LIVES HERE: this class holds no device, no
            // sampler, no binding layout, no buffer, no pipeline and no
            // binding set. Everything below is CPU-side batching state.

            // The material table: 0..2 built-ins, registered entries after.
            std::vector<MaterialEntry> m_materials;
            // Sticky globals: SetGlobals writes them and Drain() hands
            // their address out as Batch2DDrained::globals for the recorder.
            GlobalParams m_globals{};          // sticky, host-set per frame

            glm::vec2 m_viewport{ 0.0f };
            std::vector<Vertex> m_vertices;
            std::vector<uint32_t> m_indices;
            std::vector<DrawRecord> m_records;
            std::vector<BatchRun> m_runs;
            // Asset Guid -> slot index, first-use order, cleared every
            // Begin().
            std::unordered_map<Guid, uint16_t> m_textureSlotLookup;
            uint16_t m_layer = 0;
            uint16_t m_order = 0;
            // End()'s records-nothing refusal, said once for the whole run.
            bool m_warnedDevicelessEnd = false;
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
