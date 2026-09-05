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
        // ArtifactReader.hpp's FindArtifactForGuid). Content is ARTIFACT-ONLY as of
        // Task 8 (the sprite cutover): a guid whose source has no cooked artifact is the
        // ArtifactMissing refusal -- loud, memoized, latched -- exactly like a REFUSED
        // artifact (present but invalid; see PixelsFor's refusal paragraph below). Null
        // on an invalid/unresolvable id, any refusal, or an unresolvable source (logged
        // once, memoized).
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
        // true dims). Content is ARTIFACT-ONLY as of Task 8: a guid with no cooked
        // artifact is the ArtifactMissing refusal (loud, memoized, latched) -- the old
        // full-stb-decode fallback is retired; the stb symbols survive in this facade's
        // TU for the verify/compare oracle and editor chrome only. Resolves `id` through the installed
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
        // Missing agrees with the other two accessors as of Task 8: a guid
        // whose source has NO cooked artifact is the ArtifactMissing refusal,
        // loud and MEMOIZED (the Task 7-era every-call re-scan is retired).
        // PendingCook -> Resident promotion for the editor's drop-a-png flow
        // rides INVALIDATION instead: Task 12's cook-completion callback
        // invalidates this facade's entry for the cooked guid, clearing the
        // memo, and NriTextureCache's next throttled re-poll resolves the
        // fresh artifact.
        //
        // Null for an invalid/unresolvable id, a REFUSED artifact, or a guid
        // with no cooked artifact at all. The returned pointer is owned by
        // this facade's LRU-budgeted cache and is valid only until evicted --
        // callers that need it to outlive the current call must copy it, not
        // hold the pointer (same contract as PixelsFor).
        virtual const LoadedClientArtifact* ArtifactFor(const Guid& id) = 0;

        // F2b Task 12: drops every memoized entry (pixels/textureInfo/
        // artifacts, success OR memoized refusal alike) this facade holds for
        // `id` -- the un-latch a background cook's completion needs. Since
        // Task 8, a guid with no cooked artifact yet is a MEMOIZED
        // ArtifactMissing refusal at all three accessors (RefuseArtifact's
        // own comment); without this call that memo is sticky FOREVER, and a
        // texture dropped into Content/ mid-session would never promote past
        // its first "not cooked yet" ask even after the cook queue finishes.
        // The editor's cook-completion callback calls this for every guid a
        // CookSession::CookProject pass reports as freshly cooked
        // (CookResult::cookedGuids), THEN refreshes the render-side caches
        // (NriTextureCache::Invalidate, NriGraphContext::
        // InvalidateMeshAlbedoSlot) that separately memoize their OWN view of
        // the same guid downstream of this facade.
        //
        // Appended at the END of the interface (ABI v21, same-arc addition --
        // see ArtifactFor's own "NEW VIRTUALS GO AT THE END" precedent):
        // moves no existing vtable slot, but the class's shape changed again
        // this arc, so ReferenceProject.slnx needs the same rebuild Task 7
        // required for ArtifactFor.
        //
        // A no-op for an id nothing has ever resolved/refused (nothing to
        // drop). Safe to call speculatively -- e.g. for a guid whose cook
        // just FAILED too, so a stale success memo from before the edit
        // cannot linger.
        virtual void InvalidateArtifact(const Guid& id) = 0;

        // F2b desk-fix 2 (the cook-pending quiet seam): Guid -> "is a cook
        // plausibly still pending for this asset, so a MISSING resolution right
        // now should stay quiet rather than refuse loudly". Installs (or clears,
        // with an empty std::function) the facade's own cook-pending probe --
        // distinct from NriTextureCache::SetCookPendingOracle (a render-layer
        // seam with the same shape, consulted after this one has already
        // decided whether to log/latch at all).
        //
        // THE PROBLEM THIS CLOSES: opening an uncooked project resolves the
        // scene on frame 1, strictly before the editor's ~1 Hz watcher has even
        // taken its first tick (EditorAppProject.cpp's PollAssetWatch). Every
        // texture guid the scene references is genuinely Missing at that
        // instant -- not broken, just not cooked YET -- but until this seam
        // existed, TextureInfoFor/PixelsFor/ArtifactFor refused it exactly like
        // a permanently-broken artifact: an ERROR log, a memoized failure, and
        // the process-wide ContentArtifactRefusalObserved latch, ALL for a
        // condition that resolves itself within the same second once the first
        // CookProject pass lands. The editor's own cook-completion invalidation
        // (InvalidateArtifact above) was built to CLEAR that latch after the
        // fact; this seam exists so the latch, the memo and the log never fire
        // for this transient condition IN THE FIRST PLACE.
        //
        // CONSULTED ONLY on ArtifactRefusal::Missing (no cooked artifact at
        // all) -- NEVER on HashMismatch or VersionNewerThanEngine. A
        // present-but-invalid artifact is broken regardless of whether a cook
        // is queued (RefuseArtifact's own "refuse, never limp" contract is
        // unchanged for those two kinds).
        //
        // WHEN INSTALLED AND IT ANSWERS true for a Missing resolution, the
        // accessor returns null QUIETLY: no ARC_ERROR, no AssetCache::
        // PutFailure memo, no NoteContentArtifactRefusal latch. Deliberately NO
        // memo -- there is nothing to un-latch when the cook lands, because
        // nothing was latched; the very next ask re-runs ResolveArtifact from
        // scratch and picks up the fresh artifact on its own. (On the render
        // path this is cheap in practice because NriTextureCache::
        // ResolveArtifactKey's own PendingCook branch throttles how often IT
        // re-asks this facade -- see kPendingCookRepollInterval's comment; a
        // caller that asks every frame with no throttle of its own pays for a
        // fresh ResolveArtifact scan every time it is still pending, the same
        // cost class InvalidateArtifact's promotion path already accepts.)
        //
        // WHEN ABSENT (the default -- every non-editor host, and the editor
        // itself before a project is open) or when it answers false, behavior
        // is EXACTLY today's: loud, memoized, latched, unaffected. RuntimeApp
        // and every existing [assets]/[artifact] test that never installs one
        // are untouched by this addition.
        //
        // THREADING: called from whatever thread first resolves the guid --
        // today always the main thread, the SAME contract
        // SetArtifactRefusalObserver's own doc comment states (every Assets
        // accessor is reached from scene resolution / NriTextureCache::Resolve,
        // both main-thread-only by their own contracts). A probe closure that
        // reads shared state (the editor's own settling flag + its
        // m_cookDiagnostics-backed IsCookPending) relies on that same
        // invariant, exactly as m_cookDiagnostics' own "no cross-thread access
        // is ever reachable" comment already documents for its reader.
        //
        // Appended at the END of the interface (ABI v21, same-arc addition --
        // see ArtifactFor/InvalidateArtifact's own "NEW VIRTUALS GO AT THE END"
        // precedent, which this mirrors): moves no existing vtable slot, but
        // the class's shape changed again this arc, so ReferenceProject.slnx
        // needs the same rebuild those two additions required.
        virtual void SetCookPendingProbe(std::function<bool(const Guid&)> probe) = 0;
    };

    // -----------------------------------------------------------------
    // The per-refusal observer (F2b Task 12)
    // -----------------------------------------------------------------
    //
    // ContentArtifactRefusalObserved/Detail above latch the FIRST refusal
    // only -- enough for a host that exits on the first hit, not enough for
    // a long-lived editing session that wants to show EVERY refused guid in
    // its Problems pane. This fires on EVERY RefuseArtifact call (all three
    // kinds -- "ArtifactMissing", "HashMismatch", "VersionNewerThanEngine" --
    // the same `kind` strings the latch's detail string embeds), for every
    // distinct refusal, not just the process's first.
    //
    // Raw function pointer + user data, mirroring Diagnostics::SetSink --
    // keeps the DLL boundary free of std::function's allocator coupling
    // (Diagnostics.hpp's own comment on why). Install (or clear, with
    // nullptr) the process-wide observer; last writer wins, same "at most
    // one" contract as SetSink. Called from whatever thread first resolves
    // the refused guid -- today always the main thread (every Assets
    // accessor is called from scene resolution / NriTextureCache::Resolve,
    // both main-thread-only by their own contracts), but the install/read is
    // still mutex-guarded rather than relying on that.
    using ArtifactRefusalObserver = void (*)(const Guid& id, const char* kind, void* user);
    ARCANE_API void SetArtifactRefusalObserver(ArtifactRefusalObserver observer, void* user);

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
