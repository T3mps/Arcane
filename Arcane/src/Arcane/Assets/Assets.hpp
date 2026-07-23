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

    struct AssetsDesc
    {
        // Facade-wide byte budget across ALL caches (textures + bytes +
        // JSON). On insert, least-recently-used unpinned entries are evicted
        // -- cross-cache, on one shared recency clock -- until the total is
        // back under budget. The budget is strict: an asset larger than the
        // whole budget is served to the caller but swept right back out
        // (never cached). Memoized failures cost ~0 bytes and are never
        // evicted by the sweep. 0 disables eviction (unbounded, the legacy
        // contract). Default: 256 MiB -- roughly 16 uncompressed 2048^2 RGBA
        // atlases, generous for the 2D engine while still bounding growth.
        uint64_t byteBudget = 256ull * 1024 * 1024;
    };

    class ARCANE_API Assets
    {
    public:
        static std::unique_ptr<Assets> Create(nvrhi::IDevice* device,
                                              const AssetsDesc& desc = {});
        virtual ~Assets() = default;

        // Bind (or rebind) the render device used for texture uploads. The facade
        // may be constructed device-less (headless / before the host creates its
        // device); the host calls this once the device exists so GetTexture works.
        // Passing a different (or first) device invalidates any cached textures --
        // they are bound to the previous device -- and any device-less failures
        // memoized before a device was available.
        virtual void SetDevice(nvrhi::IDevice* device) = 0;

        // Base directory prepended to RELATIVE paths in GetTexture/GetBytes/GetJson;
        // absolute paths pass through unchanged. The host sets this from the open
        // project's game:// mount (project Content/) so loose-file loads resolve under
        // the project instead of exe-relative. Empty (default) == the legacy
        // exe-relative behavior. Slice 2 (AssetRegistry/GUID) resolves behind the
        // AssetId seam and this becomes the fallback for legacy path loads.
        virtual void SetContentRoot(const std::filesystem::path& root) = 0;

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
