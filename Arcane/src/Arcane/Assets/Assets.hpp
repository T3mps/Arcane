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

        // Bind (or rebind) the render device used for texture uploads. The facade
        // may be constructed device-less (headless / before the host creates its
        // device); the host calls this once the device exists so GetTexture works.
        // Passing a different (or first) device invalidates any cached textures --
        // they are bound to the previous device -- and any device-less failures
        // memoized before a device was available.
        virtual void SetDevice(nvrhi::IDevice* device) = 0;

        // Color texture (sRGB). Null on failure (logged once, memoized). Also
        // null (no crash) when no render device has been set yet.
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
