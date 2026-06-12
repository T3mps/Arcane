#pragma once

// Assets module: synchronous loose-file loaders behind cache.lua-semantics
// bookkeeping (refcount/bytes/LRU, memoized failures -- a failed path logs
// once and never retries). Color textures upload as sRGB so sampling
// yields linear (the all-linear-canvas contract). Async/streaming/BCn are
// later milestones (north star).

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>
#include <Json.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace Arcane
{
    struct AssetStats
    {
        uint64_t totalBytes = 0;
        uint32_t count = 0;
    };

    class ARCANE_API Assets
    {
    public:
        static std::unique_ptr<Assets> Create(nvrhi::IDevice* device);
        virtual ~Assets() = default;

        // Color texture (sRGB). Null on failure (logged once, memoized).
        virtual nvrhi::TextureHandle GetTexture(
            const std::filesystem::path& path) = 0;

        // Raw file bytes (fonts, blobs). Null on failure (memoized).
        virtual std::shared_ptr<const std::vector<uint8_t>> GetBytes(
            const std::filesystem::path& path) = 0;

        // Parsed JSON document (UI/data files). Null on failure (memoized).
        virtual std::shared_ptr<const nlohmann::json> GetJson(
            const std::filesystem::path& path) = 0;

        virtual AssetStats Stats() const = 0;
    };
}
