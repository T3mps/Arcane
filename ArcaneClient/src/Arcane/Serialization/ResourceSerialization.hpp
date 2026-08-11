#pragma once

// Serializable-resource persistence (opt-in).
//
// Astra's Registry::Save() serializes entities/components/relations but NOT
// resources (ResourceStorage), so every hot-reload silently lost SetResource
// values -- plugins only survived by hand-persisting the SceneRoot entity id.
//
// This is the engine-side companion: a small registration seam of resource
// CODECS keyed by the resource type's stable name hash. Only explicitly
// registered resources persist -- SceneRoot (a plain Astra::Entity id, which
// survives Registry::Save/Load intact) is registered by default; live host/GPU
// pointer resources (SpriteTable, RenderContext2D) are deliberately NOT, since
// they must be re-established by the host on load.
//
// The section is length-framed per entry so a loader can skip an unknown
// resource type (forward-compat) and a corrupt/truncated section fails with a
// clean SerializationError rather than a throw.
//
//   section := [u32 count] { [u64 typeHash][u32 bodyLen][body ...] }*

#include <Arcane/Base/Api.hpp>

#include <Astra/Core/Result.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/SerializationError.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Arcane::Serialization
{
    // One resource type's persistence hooks. Function pointers (not std::function)
    // keep this ABI-trivial across the Arcane.dll boundary. save() returns false
    // when the resource is absent on the registry (nothing written -- skipped).
    struct ResourceCodec
    {
        uint64_t typeHash = 0;
        bool (*save)(const Astra::Registry&, Astra::BinaryWriter&) = nullptr;
        bool (*load)(Astra::Registry&, Astra::BinaryReader&)       = nullptr;
    };

    // Ordered set of resource codecs. Idempotent by typeHash. Lives as a single
    // process-wide instance (SerializableResources()); a host or a test can
    // register additional serializable resource types into it.
    class ResourceSerializerRegistry
    {
    public:
        void Register(const ResourceCodec& codec)
        {
            for (ResourceCodec& e : m_codecs)
                if (e.typeHash == codec.typeHash) { e = codec; return; }
            m_codecs.push_back(codec);
        }

        ASTRA_NODISCARD const ResourceCodec* Find(uint64_t typeHash) const
        {
            for (const ResourceCodec& e : m_codecs)
                if (e.typeHash == typeHash) return &e;
            return nullptr;
        }

        ASTRA_NODISCARD const std::vector<ResourceCodec>& Codecs() const noexcept { return m_codecs; }

    private:
        std::vector<ResourceCodec> m_codecs;
    };

    // The process-wide serializable-resource set (defined in Arcane.dll). Default-
    // registers the SceneRoot codec on first access. Exported so a test/host in
    // another module registers into the SAME instance the engine snapshots with.
    ARCANE_API ResourceSerializerRegistry& SerializableResources();

    // Serializes every registered + present resource into a length-framed section.
    inline std::vector<std::byte> WriteResourceSection(const Astra::Registry& reg,
                                                       const ResourceSerializerRegistry& set)
    {
        std::vector<std::byte> body;
        Astra::BinaryWriter bodyW(body);
        uint32_t count = 0;

        for (const ResourceCodec& c : set.Codecs())
        {
            if (!c.save) continue;

            std::vector<std::byte> one;
            {
                Astra::BinaryWriter oneW(one);
                if (!c.save(reg, oneW)) continue;   // resource absent -> skip
                if (oneW.HasError())    continue;   // codec write failed -> skip (defensive)
            }
            bodyW(c.typeHash);
            bodyW(static_cast<uint32_t>(one.size()));
            bodyW.WriteBytes(one.data(), one.size());
            ++count;
        }

        std::vector<std::byte> out;
        Astra::BinaryWriter outW(out);
        outW(count);
        outW.WriteBytes(body.data(), body.size());
        return out;
    }

    // Applies a section to a registry: dispatches each framed entry to its codec
    // by typeHash, skipping unknown types. Never throws; a corrupt/truncated
    // section returns a clean SerializationError.
    inline Astra::Result<void, Astra::SerializationError>
    ReadResourceSection(Astra::Registry& reg, std::span<const std::byte> bytes,
                        const ResourceSerializerRegistry& set)
    {
        using R = Astra::Result<void, Astra::SerializationError>;

        if (bytes.empty()) return R::Ok();   // no section present -> nothing to restore

        Astra::BinaryReader r(bytes);
        uint32_t count = 0; r(count);
        if (r.HasError())
            return R::Err(Astra::SerializationError::CorruptedData);

        for (uint32_t i = 0; i < count; ++i)
        {
            uint64_t hash = 0; uint32_t bodyLen = 0;
            r(hash); r(bodyLen);
            if (r.HasError())
                return R::Err(Astra::SerializationError::CorruptedData);
            if (bodyLen > r.GetRemaining())
                return R::Err(Astra::SerializationError::CorruptedData);

            const std::size_t bodyStart = r.GetPosition();
            if (const ResourceCodec* c = set.Find(hash); c && c->load)
            {
                if (!c->load(reg, r))
                    return R::Err(Astra::SerializationError::CorruptedData);
            }
            else
            {
                r.Skip(bodyLen);   // unknown resource type -> skip its body
            }
            if (r.HasError())
                return R::Err(Astra::SerializationError::CorruptedData);

            // Re-sync to the framed body end so a codec that under-reads cannot
            // desync the next entry; over-read means the frame lied -> corrupt.
            const std::size_t consumed = r.GetPosition() - bodyStart;
            if (consumed < bodyLen)      r.Skip(bodyLen - consumed);
            else if (consumed > bodyLen) return R::Err(Astra::SerializationError::CorruptedData);
        }
        return R::Ok();
    }
}
