#pragma once

// Assets module: synchronous loose-file loaders behind cache.lua-semantics
// bookkeeping (refcount/bytes/LRU, memoized failures -- a failed path logs
// once and never retries). Color textures upload as sRGB so sampling
// yields linear (the all-linear-canvas contract). Async/streaming/BCn are
// later milestones (north star).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Project/AssetId.hpp>

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
        // Facade-wide byte budget across ALL caches (textures + bytes + JSON
        // + decoded pixels). On insert, least-recently-used unpinned entries
        // are evicted -- cross-cache, on one shared recency clock -- until
        // the total is back under budget. The budget is strict: an asset
        // larger than the whole budget is served to the caller but swept
        // right back out (never cached). Memoized failures cost ~0 bytes and
        // are never evicted by the sweep. 0 disables eviction (unbounded,
        // the legacy contract). Default: 256 MiB -- roughly 16 uncompressed
        // 2048^2 RGBA atlases, generous for the 2D engine while still
        // bounding growth.
        uint64_t byteBudget = 256ull * 1024 * 1024;
    };

    // THE FACADE IS DEVICE-FREE. It owns no render device, uploads nothing,
    // and hands out no texture object: PixelsFor(Guid) below is the
    // device-free supply, and the graph path's NriTextureCache is what puts
    // those pixels on a device.
    class ARCANE_API Assets
    {
    public:
        static std::unique_ptr<Assets> Create(const AssetsDesc& desc = {});
        virtual ~Assets() = default;

        // Base directory prepended to RELATIVE paths in the PATH overloads of
        // GetBytes/GetJson; absolute paths pass through unchanged. The
        // host sets this from the open project's game:// mount (project Content/)
        // so loose-file loads resolve under the project instead of exe-relative.
        // Empty (default) == the legacy exe-relative behavior.
        //
        // ANCHORING CONVENTION -- it applies to CALLER-SUPPLIED paths ONLY. It is
        // never applied to a path the AssetResolver returned: those are already
        // load-ready (see SetAssetResolver). Anchoring both ends is what produced
        // "<root>/Content/<root>/Content/..." for a relatively-opened project.
        virtual void SetContentRoot(const std::filesystem::path& root) = 0;

        // The GUID resolution seam: AssetId -> physical file. The host installs the
        // open project's resolver (Project::ResolveAsset wraps AssetRegistry + the
        // MountTable); the AssetId overloads below route through it into the SAME
        // cached loaders as the path overloads. Installing a resolver (or a new one)
        // clears the unresolved-id memos so a rescan gets a clean retry. No resolver
        // (default) fails every AssetId load with one warning.
        //
        // CONTRACT: the resolver returns a LOAD-READY path -- one that opens as-is
        // (relative results are relative to the process CWD, like any ifstream).
        // The MountTable has already joined the asset onto its OWN mount root, and
        // those roots are not all under the content root (diag:// is <project>/
        // Saved/Diagnostics, plugin/<x>:// is <plugin>/Content), so this facade
        // must not re-anchor the result. Every other consumer of the same resolver
        // -- SpriteCache, SpriteMaterialCache, PostChainCache, ProjectBoot, the
        // editor, the NRI graph vehicle -- already opens it verbatim.
        using AssetResolver =
            std::function<std::optional<std::filesystem::path>(const AssetId&)>;
        virtual void SetAssetResolver(AssetResolver resolver) = 0;

        // Decoded pixels for a texture asset, device-free -- THE facade's
        // image supply since ABI v15 removed GetTexture. Resolves `id` through
        // the installed AssetResolver, then decodes once and retains the
        // result: a second call for the same id is a cache hit, returning the
        // SAME pointer. Null on an invalid/unresolvable id or a decode failure
        // (logged once, memoized). The returned pointer is owned by the
        // facade's LRU-budgeted pixel cache (bytes-weighted) and is valid only
        // until evicted -- callers that need it to outlive the current call
        // must copy it, not hold the pointer.
        virtual const PixelData* PixelsFor(const Guid& id) = 0;

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

    // NOTHING BELOW TAKES A DEVICE OR A TEXTURE OBJECT. Reading a rendered
    // image back is NriGraphContext::ReadCapture's job; everything here is
    // the CPU half -- resolve, decode, repack, write.

    // THE UI-IMAGE LOADER: the exe-relative resolve, the decode and the
    // maxSize area-average downscale, with NO device.
    //
    // It exists because the graph path uploads through a different route: an
    // editor chrome image (the toolbar logo) reaches the GPU through
    // NriTextureCache with ColorSpace::Display, which takes its bytes from a
    // PixelSupplyFn rather than from a file. Same relationship
    // RepackStagingToRgba has to the PNG writer -- one definition of what the
    // pixels ARE, independent of how they get onto a device.
    //
    // `out` is left default-constructed (i.e. !Valid()) on any failure, which
    // is WARN-logged, never ERROR (a missing UI image must not trip the GPU
    // tests' RenderErrorCount()==0 gate). maxSize (0 = off) caps the LARGER
    // dimension, aspect preserved -- the loader's rule, not the thumbnail
    // writer's width cap.
    ARCANE_API bool LoadDisplayPixels(
        const std::filesystem::path& path, uint32_t maxSize, PixelData& out);

    // Repack mapped staging rows (rowPitch may exceed w*4) into a tight RGBA
    // buffer, swizzling when the source rows are BGRA (the OffscreenCanvas
    // output order) and forcing alpha OPAQUE either way -- a screenshot is a
    // picture of the screen, and whatever coverage math left in the target's
    // alpha channel must not punch holes in it. Exported so that byte-order
    // contract is unit-testable without a device, which is now the only way it
    // is exercised: ReadTexturePixels, its one caller, went at ABI v15.
    ARCANE_API void RepackStagingToRgba(
        const unsigned char* src, size_t rowPitch, uint32_t width, uint32_t height,
        bool bgraSource, std::vector<unsigned char>& out);

    // THE WHOLE OF "WRITE THIS IMAGE AS A COVER THUMBNAIL" -- the width cap,
    // the area-average downscale, the opaque-alpha rule and the PNG write.
    // Exported for the same reason RepackStagingToRgba above is, and it is
    // the ONLY cover writer: a rendered image arrives through
    // NriGraphContext::ReadCapture (already tight, already BGRA-normalized)
    // and lands here.
    //
    // `rgba` is TIGHT RGBA8 (no row padding), `width` x `height`. maxWidth
    // (0 = off) caps the WIDTH -- not the larger dimension, unlike the loader
    // -- because the consumer is a fixed-width thumbnail tile and the source
    // is a viewport whose aspect the user chose. Alpha is forced OPAQUE, the
    // same rule and the same reason RepackStagingToRgba states. Parent
    // directories are created. False on failure, logged as WARN, never ERROR.
    ARCANE_API bool WriteThumbnailPngRgba(
        const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
        std::vector<unsigned char> rgba, uint32_t maxWidth = 0);
}
