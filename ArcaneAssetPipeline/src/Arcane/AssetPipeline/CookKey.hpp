#pragma once

// Arcane::AssetPipeline::CookKey -- the triple cook key: hash(source bytes + import settings +
// importer version). Deterministic across runs/platforms (spec s4 contract): both builders below
// hash EXPLICIT FIELDS of their own kind's *MetaSettings and importerVersion one byte at a time,
// never a struct memcpy -- struct padding is compiler/ABI dependent and would make the key
// nondeterministic across toolchains, which breaks content-hash-based caching (the same "struct
// padding RNG" trap ArtifactFormat's writer rule cites, UE TextureCompressorModule.cpp:
// 3828-3880). No std::hash anywhere in this path -- it is implementation-defined and MUST NOT
// be used for anything that lands on disk or is compared across processes/runs.
//
// F2c Task 5 widened this file to carry TWO kind-specific key builders -- ComputeCookKey
// (texture) and ComputeMeshCookKey (mesh) -- over the SAME hash primitive (the file-local
// Fnv1a64 in CookKey.cpp). This is exactly what spec R6 ("generalize the spine, per-kind
// leaves") prescribes for the cook key specifically: the spine shares the hash algorithm and
// the triple's SHAPE (source bytes, settings, importer version), but never the field list --
// each kind's builder hashes its own *MetaSettings' own fields, explicitly, and the mesh
// builder additionally folds in every external buffer a source can reference (see
// ComputeMeshCookKey below), a term the texture key has no analogue for. Texture behavior is
// unchanged by this widening -- ComputeCookKey's signature, body, and existing callers are
// untouched.

#include <cstddef>
#include <cstdint>
#include <span>

#include "Arcane/AssetPipeline/MeshMetaSettings.hpp"
#include "Arcane/AssetPipeline/TextureMetaSettings.hpp"

namespace Arcane::AssetPipeline
{
    // COMPOSITE version: bump when importer logic, bc7enc_rdo, or stb change. Constituents
    // named here so no one forgets the third term of the triple.
    inline constexpr std::uint32_t kTextureImporterVersion = 1;   // {importer v1, bc7enc_rdo b943862, stb 31c1ad3}

    [[nodiscard]] std::uint64_t ComputeCookKey(std::span<const std::byte> sourceBytes,
                                                const TextureMetaSettings& settings,
                                                std::uint32_t importerVersion);

    // COMPOSITE version: bump when importer logic, cgltf, or meshoptimizer change.
    inline constexpr std::uint32_t kMeshImporterVersion = 1;   // {importer v1, cgltf bbeb5b0, meshoptimizer v1.2}

    // hash(u32 source length + source bytes + EVERY external buffer's bytes (each its own
    // u32 length then the bytes) + settings fields + importer version). The external-buffer
    // term is spec s5.4 and is load-bearing: a .gltf referencing "geometry.bin" changes
    // NOTHING in its own bytes when that buffer is re-exported, so a key over the .gltf
    // alone would serve a stale artifact forever. Buffers are fed in glTF DECLARATION
    // ORDER, each preceded by its own u32 length, so two buffers can never be confused for
    // one longer one -- and the LEADING source-length prefix anchors the source/buffer-list
    // boundary for the same reason: without it, {sourceBytes, no buffers} and {shorter
    // sourceBytes, one buffer that absorbs the missing tail} feed the identical byte stream
    // (the encoding is injective end to end once every variable-length region -- source
    // included -- carries its own length ahead of it).
    //
    // External IMAGES are deliberately NOT here: they are their own registered texture
    // assets with their own cook keys (s5.4), and folding them in would recook the
    // geometry every time an artist touched a texture.
    [[nodiscard]] std::uint64_t ComputeMeshCookKey(
        std::span<const std::byte> sourceBytes,
        std::span<const std::span<const std::byte>> externalBuffers,
        const MeshMetaSettings& settings,
        std::uint32_t importerVersion);
}
