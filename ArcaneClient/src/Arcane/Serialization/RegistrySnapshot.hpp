#pragma once

// Registry snapshot framing + error propagation.
//
// A snapshot is the Astra registry blob (entities/components/relations) plus an
// engine-side serializable-resource section (ResourceSerialization.hpp). Astra's
// Registry::Save() drops all resources, so SnapshotRegistry frames the two
// together:
//
//   [u32 magic 'ARSS'][u16 version][u32 registryLen][registry blob][resource section]
//
// FinishSnapshot propagates a Registry::Save() failure as a Result error instead
// of the old empty-but-"ok" vector that masked data loss (E02-4). FrameBytes
// concatenates the two payloads behind the header; ParseSnapshot splits a frame
// back into its two spans, rejecting a wrong-magic / wrong-version / truncated
// buffer with a clean SerializationError (no throw, no crash).

#include <Astra/Core/Result.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>
#include <Astra/Serialization/SerializationError.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace Arcane::Serialization
{
    using SnapshotResult = Astra::Result<std::vector<std::byte>, Astra::SerializationError>;

    // 'A','R','S','S' as little-endian bytes; bumps if the frame layout changes.
    inline constexpr uint32_t kSnapshotMagic   = 0x53535241u;
    inline constexpr uint16_t kSnapshotVersion = 1u;

    // Byte offset where the registry blob begins: magic(4) + version(2) + len(4).
    inline constexpr std::size_t kSnapshotHeaderSize = sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint32_t);

    // Propagates a Registry::Save() failure as a Result error; on success yields
    // the raw registry bytes unchanged. Pure + header-only so callers AND tests
    // can drive the failure path deterministically (Registry::Save() to memory is
    // otherwise infallible, so this seam is how the propagation contract is
    // exercised).
    inline SnapshotResult FinishSnapshot(SnapshotResult&& saveResult)
    {
        if (saveResult.IsErr())
            return SnapshotResult::Err(*saveResult.GetError());
        return SnapshotResult::Ok(std::move(*saveResult.GetValue()));
    }

    // Concatenates a registry blob + resource section behind the frame header.
    // Non-failing (the inputs are already-serialized buffers).
    inline std::vector<std::byte> FrameBytes(const std::vector<std::byte>& registryBlob,
                                             const std::vector<std::byte>& resourceSection)
    {
        // Frame-format limit: the registry-blob length field is uint32, so a
        // blob >= 4 GiB cannot be framed -- the cast below would silently
        // truncate the length and desync the resource-section offset on parse.
        // Far beyond any realistic registry; assert rather than widen the format.
        assert(registryBlob.size() <= std::numeric_limits<uint32_t>::max()
               && "RegistrySnapshot: registry blob exceeds the uint32 frame length field");

        std::vector<std::byte> out;
        out.reserve(kSnapshotHeaderSize + registryBlob.size() + resourceSection.size());
        Astra::BinaryWriter w(out);
        w(kSnapshotMagic);
        w(kSnapshotVersion);
        w(static_cast<uint32_t>(registryBlob.size()));
        w.WriteBytes(registryBlob.data(), registryBlob.size());
        w.WriteBytes(resourceSection.data(), resourceSection.size());
        return out;
    }

    // Two views into a parsed snapshot frame (borrowed from the input span).
    struct SnapshotFrame
    {
        std::span<const std::byte> registry;
        std::span<const std::byte> resources;
    };

    // Splits a snapshot frame into (registry blob, resource section) views, or a
    // clean error for a wrong-magic / wrong-version / truncated buffer.
    inline Astra::Result<SnapshotFrame, Astra::SerializationError>
    ParseSnapshot(std::span<const std::byte> bytes)
    {
        using R = Astra::Result<SnapshotFrame, Astra::SerializationError>;

        if (bytes.size() < kSnapshotHeaderSize)
            return R::Err(Astra::SerializationError::CorruptedData);

        Astra::BinaryReader r(bytes);
        uint32_t magic = 0; r(magic);
        if (r.HasError() || magic != kSnapshotMagic)
            return R::Err(Astra::SerializationError::InvalidMagic);
        uint16_t version = 0; r(version);
        if (r.HasError())
            return R::Err(Astra::SerializationError::CorruptedData);
        if (version != kSnapshotVersion)
            return R::Err(Astra::SerializationError::UnsupportedVersion);
        uint32_t registryLen = 0; r(registryLen);
        if (r.HasError())
            return R::Err(Astra::SerializationError::CorruptedData);

        const std::size_t registryStart = r.GetPosition();   // == kSnapshotHeaderSize
        if (registryLen > bytes.size() - registryStart)
            return R::Err(Astra::SerializationError::CorruptedData);

        SnapshotFrame frame;
        frame.registry  = bytes.subspan(registryStart, registryLen);
        frame.resources = bytes.subspan(registryStart + registryLen);
        return R::Ok(frame);
    }
}
