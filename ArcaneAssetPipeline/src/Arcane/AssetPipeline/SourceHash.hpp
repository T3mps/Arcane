#pragma once

// SourceHash.hpp -- FNV-1a 64 over raw source bytes, the artifact header's
// `sourceHash` fingerprint. EXTRACTED at F2c Task 7 from TextureImporter.cpp,
// whose own copy already carried a comment flagging it as a deliberate duplicate
// of CookKey.cpp's hasher. Two copies were a documented trade; a THIRD (the mesh
// importer's) is where a drift becomes inevitable and silent -- a drifted hash
// does not fail to compile, it makes every artifact HashMismatch forever.
// CookKey.cpp's Fnv1a64 stays separate on purpose: it hashes explicit FIELDS,
// not a byte span, and merging the two would drag the field discipline into a
// function that has no fields.
//
// WHY THIS MOVE HAPPENED HERE, NOT SOONER: TextureImporter.cpp's copy was fine as
// a single occurrence carrying its own "this is a deliberate duplicate" comment.
// MeshImporter.cpp needing the SAME function is what turns "duplicated once, on
// purpose" into "duplicated twice, by drift risk" -- so this task extracts the ONE
// function both importers call, rather than hand-copying its body a third time.
//
// Two callers, two different inputs, ONE shared meaning of "the source bytes":
//   - TextureImporter.cpp hashes just `pngBytes` -- a .png has no external buffers,
//     so this move changes NOTHING about its input and every existing texture
//     artifact keeps its exact on-disk sourceHash.
//   - MeshImporter.cpp hashes the source bytes FOLLOWED BY every external buffer's
//     bytes, concatenated in glTF declaration order -- the exact concatenation
//     ArcaneClient/src/Arcane/Assets/ArtifactReader.hpp's `currentSourceBytes`
//     contract already names, so the pipeline and the client agree on what "the
//     current source bytes" means by construction, not by two functions happening
//     to match.

#include <cstddef>
#include <cstdint>
#include <span>

namespace Arcane::AssetPipeline
{
    [[nodiscard]] inline std::uint64_t HashSourceBytes(std::span<const std::byte> bytes) noexcept
    {
        std::uint64_t h = 14695981039346656037ULL;
        constexpr std::uint64_t prime = 1099511628211ULL;
        for (std::byte b : bytes)
        {
            h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(b));
            h *= prime;
        }
        return h;
    }
}
