#pragma once

// Assets module: synchronous loose-file loaders behind cache.lua-semantics
// bookkeeping (refcount/bytes/LRU, memoized failures -- a failed path logs
// once and never retries). Color textures upload as sRGB so sampling
// yields linear (the all-linear-canvas contract). Async/streaming/BCn are
// later milestones (north star).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Project/AssetId.hpp>

#include <nvrhi/nvrhi.h>
#include <Json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
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
        // exe-relative behavior. GUID loads resolve behind the AssetId seam below;
        // this stays as the fallback for legacy path loads.
        virtual void SetContentRoot(const std::filesystem::path& root) = 0;

        // The GUID resolution seam: AssetId -> physical file. The host installs the
        // open project's resolver (Project::ResolveAsset wraps AssetRegistry + the
        // MountTable); the AssetId overloads below route through it into the SAME
        // cached loaders as the path overloads. Installing a resolver (or a new one)
        // clears the unresolved-id memos so a rescan gets a clean retry. No resolver
        // (default) fails every AssetId load with one warning.
        using AssetResolver =
            std::function<std::optional<std::filesystem::path>(const AssetId&)>;
        virtual void SetAssetResolver(AssetResolver resolver) = 0;

        // Color texture (sRGB). Null on failure (logged once, memoized). Also
        // null (no crash) when no render device has been set yet.
        virtual nvrhi::TextureHandle GetTexture(
            const std::filesystem::path& path) = 0;
        virtual nvrhi::TextureHandle GetTexture(const AssetId& id) = 0;

        // Raw file bytes (fonts, blobs). Null on failure (memoized).
        virtual std::shared_ptr<const std::vector<uint8_t>> GetBytes(
            const std::filesystem::path& path) = 0;
        virtual std::shared_ptr<const std::vector<uint8_t>> GetBytes(const AssetId& id) = 0;

        // Parsed JSON document (UI/data files). Null on failure (memoized).
        virtual std::shared_ptr<const nlohmann::json> GetJson(
            const std::filesystem::path& path) = 0;
        virtual std::shared_ptr<const nlohmann::json> GetJson(const AssetId& id) = 0;

        virtual AssetStats Stats() const = 0;
    };

    // Load an image file into a DISPLAY-REFERRED (RGBA8_UNORM) texture, for ImGui/UI
    // where the sampled texel composites DIRECTLY into the display-referred target
    // (the ImGui backbuffer / a tonemapped viewport output). This is deliberately NOT
    // Assets::GetTexture, which uploads sRGB (SRGBA8_UNORM) so the sampler yields linear
    // for the all-linear scene canvas -- feeding that to ImGui double-decodes and reads
    // too dark. Uncached: the caller owns the returned handle (editor icons/logos are
    // few and long-lived, so the facade's LRU budget would only get in the way). Relative
    // paths resolve exe-relative (same anchor as Assets). Null on failure (logged once).
    //
    // maxSize (0 = off) caps the larger dimension by a one-pass area-average (box) downscale
    // BEFORE upload (aspect preserved). The ImGui sampler has no mipmaps, so a large source
    // drawn small (e.g. a 550px logo at 24px) would alias hard under single-tap bilinear.
    // Size maxSize to ~2x the on-screen draw size: the area-average removes aliasing, and the
    // UI's own bilinear then only minifies <=2x (a clean box) -- and it uploads far less VRAM.
    ARCANE_API nvrhi::TextureHandle LoadDisplayTexture(
        nvrhi::IDevice* device, const std::filesystem::path& path, uint32_t maxSize = 0);

    // The CPU half of SaveTexturePng, exported so its byte-order contract is
    // unit-testable without a device: repack mapped staging rows (rowPitch may
    // exceed w*4) into a tight RGBA buffer, swizzling when the source rows are
    // BGRA (the OffscreenCanvas output order) and forcing alpha OPAQUE either
    // way -- a screenshot is a picture of the screen, and whatever coverage
    // math left in the target's alpha channel must not punch holes in it.
    ARCANE_API void RepackStagingToRgba(
        const unsigned char* src, size_t rowPitch, uint32_t width, uint32_t height,
        bool bgraSource, std::vector<unsigned char>& out);

    // Save a GPU texture as a PNG on disk -- the writer twin of
    // LoadDisplayTexture (the editor's Saved/AutoScreenshot.png, which the
    // Arcane Hub reads as the project's cover thumbnail, rides this).
    // SYNCHRONOUS: staging copy + waitForIdle + map, the PickBuffer idiom --
    // one deliberate stall per call, so callers are rare events (a save, a
    // shutdown), never the frame loop. Accepts BGRA8_UNORM (the tonemapped
    // OffscreenCanvas output; swizzled) and RGBA8_UNORM; anything else is
    // refused. maxWidth (0 = off) caps the width via the same area-average
    // downscale the loader uses (aspect preserved) -- a thumbnail should not
    // ship megabytes. Parent directories are created. False on any failure,
    // logged as WARN, never ERROR: a screenshot that cannot be written must
    // not trip the GPU tests' RenderErrorCount()==0 gate.
    ARCANE_API bool SaveTexturePng(
        nvrhi::IDevice* device, nvrhi::ITexture* texture,
        const std::filesystem::path& path, uint32_t maxWidth = 0);
}
