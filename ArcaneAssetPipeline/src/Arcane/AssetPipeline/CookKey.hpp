#pragma once

// Arcane::AssetPipeline::CookKey -- the triple cook key: hash(source bytes + import settings +
// importer version). Deterministic across runs/platforms (spec s4 contract): ComputeCookKey
// hashes EXPLICIT FIELDS of TextureMetaSettings and importerVersion one byte at a time, never a
// struct memcpy -- struct padding is compiler/ABI dependent and would make the key
// nondeterministic across toolchains, which breaks content-hash-based caching (the same "struct
// padding RNG" trap ArtifactFormat's writer rule cites, UE TextureCompressorModule.cpp:
// 3828-3880). No std::hash anywhere in this path -- it is implementation-defined and MUST NOT
// be used for anything that lands on disk or is compared across processes/runs.

#include <cstddef>
#include <cstdint>
#include <span>

#include "Arcane/AssetPipeline/TextureMetaSettings.hpp"

namespace Arcane::AssetPipeline
{
    // COMPOSITE version: bump when importer logic, bc7enc_rdo, or stb change. Constituents
    // named here so no one forgets the third term of the triple.
    inline constexpr std::uint32_t kTextureImporterVersion = 1;   // {importer v1, bc7enc_rdo b943862, stb 31c1ad3}

    [[nodiscard]] std::uint64_t ComputeCookKey(std::span<const std::byte> sourceBytes,
                                                const TextureMetaSettings& settings,
                                                std::uint32_t importerVersion);
}
