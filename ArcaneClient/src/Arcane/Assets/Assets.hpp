#pragma once

// Assets module: synchronous loose-file loaders behind cache.lua-semantics
// bookkeeping (refcount/bytes/LRU, memoized failures -- a failed path logs
// once and never retries). Color textures upload as sRGB so sampling
// yields linear (the all-linear-canvas contract). Async/streaming/BCn are
// later milestones (north star).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Assets/ArtifactReader.hpp>   // TextureInfo -- TextureInfoFor's payload
#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Project/AssetId.hpp>

#include <Json.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
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

        // Header dims/mipCount/srgb for a texture asset -- ABI v21, DEVICE-FREE, artifact-
        // BACKED: for content that has a cooked .arcart (F2b Task 5's arccook; see
        // ArtifactReader.hpp), this reads the artifact's HEADER, never a decode -- cheap
        // even for a texture whose pixels have never been touched this session. Resolves
        // `id` through the installed AssetResolver to find the CONTENT source, then finds
        // that source's artifact under the project's Intermediate/Artifacts (see
        // ArtifactReader.hpp's FindArtifactForGuid); a guid whose source has no cooked
        // artifact yet falls back to a plain stb dimension probe of the source file itself
        // (Task 8 retires this fallback once every content texture is guaranteed a cooked
        // artifact). Null on an invalid/unresolvable id, a REFUSED artifact (present but
        // invalid -- see PixelsFor's own refusal paragraph below, same memoization, same
        // "refuse, never limp" posture), or an unresolvable source (logged once, memoized).
        //
        // THIS IS THE DIMENSION SOURCE going forward -- SpriteCache.cpp reads geometry dims
        // from here, not from PixelsFor, specifically because PixelsFor now serves a
        // THUMBNAIL for artifact-backed content: the two calls answer DIFFERENT questions
        // (true source dims vs. preview-pixel dims) for the SAME guid, and conflating them
        // was the wrong-dims bug class this split exists to prevent.
        virtual const TextureInfo* TextureInfoFor(const Guid& id) = 0;

        // Preview pixels for a texture asset, device-free -- ABI v21 NARROWS this contract:
        // for artifact-backed content (a guid whose source has a cooked .arcart) this
        // serves the artifact's own THUMBNAIL (small, uncompressed RGBA8, PixelData::width/
        // height are the THUMBNAIL's dims, NOT the source's -- use TextureInfoFor above for
        // true dims). For content with no artifact yet, this keeps today's behaviour
        // unchanged: a full stb decode of the source file, PixelData::width/height are the
        // real dims (Task 8 retires this fallback, at which point every content texture
        // routes through the artifact path). Resolves `id` through the installed
        // AssetResolver, then decodes/loads once and retains the result: a second call for
        // the same id is a cache hit, returning the SAME pointer. Null on an invalid/
        // unresolvable id, an unreadable/undecodable source (logged once, memoized), or a
        // REFUSED artifact -- HashMismatch (the artifact's sourceHash disagrees with the
        // CURRENT staged source bytes: a stale/broken cook) or VersionNewerThanEngine (the
        // artifact's importer is newer than this build knows how to read). A refusal is
        // memoized the SAME way a decode failure is (never a retry storm) but is logged as
        // an ERROR, not a WARN -- "refuse, never limp": this facade will not silently serve
        // stb-decoded pixels over an artifact IT KNOWS is invalid, because that would mask
        // a broken cook rather than surface it. See
        // Arcane::ContentArtifactRefusalObserved/ContentArtifactRefusalDetail below for how
        // a host turns the first such refusal into a hard exit. The returned pointer is
        // owned by the facade's LRU-budgeted pixel cache (bytes-weighted) and is valid only
        // until evicted -- callers that need it to outlive the current call must copy it,
        // not hold the pointer.
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

        // THE COMPILED-TEXTURE SUPPLY for the render path (ABI v21, Task 7 --
        // same-arc addition, appended here rather than inserted above so a
        // stale plugin module's existing vtable offsets never move; see
        // Batcher2D.hpp's own "NEW VIRTUALS GO AT THE END" rule, which this
        // mirrors). The FULL loaded artifact -- header, format, EVERY mip's
        // view into its own payload -- for a texture asset whose source has a
        // cooked .arcart, resolved and memoized exactly like TextureInfoFor/
        // PixelsFor above (same ResolveArtifact, same refusal discipline: a
        // PRESENT-but-invalid artifact -- HashMismatch or
        // VersionNewerThanEngine -- refuses loudly and memoizes, same "refuse,
        // never limp" ERROR + latch as the other two accessors).
        //
        // UNLIKE TextureInfoFor/PixelsFor, a guid whose source has NO cooked
        // artifact YET is NOT memoized here: this accessor has no fallback to
        // offer (there is no decode-to-mips path the way PixelsFor has a
        // stb decode), so "no artifact yet" is re-checked on every call
        // rather than latched as a permanent miss. That is deliberate and
        // load-bearing for its one production consumer,
        // NriTextureCache::ArtifactSupplyFn (Task 7): a texture in the
        // engine's PendingCook state must be able to promote to Resident the
        // moment a cook queue (Task 12) produces the artifact, which a sticky
        // "missing" memo would permanently prevent. FindArtifactForGuid's own
        // directory scan is cheap per its own doc comment (ArtifactReader.hpp)
        // -- this is what makes the re-check affordable.
        //
        // Null for an invalid/unresolvable id, a REFUSED artifact, or a guid
        // with no cooked artifact at all. The returned pointer is owned by
        // this facade's LRU-budgeted cache and is valid only until evicted --
        // callers that need it to outlive the current call must copy it, not
        // hold the pointer (same contract as PixelsFor).
        virtual const LoadedClientArtifact* ArtifactFor(const Guid& id) = 0;
    };

    // -----------------------------------------------------------------
    // The process-wide content-artifact-refusal latch
    // -----------------------------------------------------------------
    //
    // "Refuse, never limp" (F2b Task 6, spec s5): TextureInfoFor/PixelsFor's artifact-
    // backed path sets this latch the FIRST time it hits a PRESENT-but-INVALID artifact
    // (HashMismatch or VersionNewerThanEngine -- never Missing, which is not yet a
    // refusal at this facade layer; see PixelsFor's own doc comment) instead of ever
    // falling back to a source-file decode for that guid. One slot, first-refusal-wins --
    // same idiom as Render/GpuInstrumentation.hpp's device-lost latch, and for the same
    // reason: one bad artifact is already a stop-the-boot event, so there is nothing to
    // gain from accumulating a list.
    //
    // ArcaneRuntime polls this once, after MainLoop (RuntimeApp::Run, mirroring
    // GpuDeviceLostObserved's own poll site), and exits nonzero naming the refusal --
    // refuse-out-loud is a HOST policy, this is only the fact the host polls. The editor
    // does NOT poll it: Task 12 publishes the SAME refusals to the Problems pane instead
    // of exiting, so an artifact refusal never takes down an editing session the way it
    // takes down a game host.
    [[nodiscard]] ARCANE_API bool ContentArtifactRefusalObserved() noexcept;

    // The first refusal's own description -- "<refusal kind>: <guid>", e.g.
    // "HashMismatch: 11111111-2222-4333-8444-555555555555" -- naming the refusal is Step
    // 3's contract ("exits nonzero with the refusal named"). Empty when
    // ContentArtifactRefusalObserved() is false.
    [[nodiscard]] ARCANE_API std::string ContentArtifactRefusalDetail();

    // TEST-ONLY reset -- clears the latch (and its detail string) back to the never-fired
    // state. Unlike GpuInstrumentation.hpp's ResetGpuDeviceLost (which pairs with a real
    // production re-arm site: a rebuilt device after a project switch legitimately clears
    // a stale device-lost verdict), NOTHING in production calls this: a content-artifact
    // refusal has no "comes back healthy" event to re-arm for -- the host that observes
    // it exits the process, and a fresh process starts with a fresh (unset) latch by
    // construction. Exists purely so ArcaneTests' [assets]/[artifact] cases can prove their
    // OWN refusal fired without inheriting an earlier, unrelated case's latch from the same
    // process (Catch2 runs every TEST_CASE in one process, random order).
    ARCANE_API void ResetContentArtifactRefusal() noexcept;

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
