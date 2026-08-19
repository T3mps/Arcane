#include <Arcane/Assets/Assets.hpp>

#include <Arcane/Assets/AssetCache.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>

#include <Json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Arcane
{
    namespace
    {
        // Resolve a path exe-relative when relative -- the engine-wide anchor
        // (Render/ShaderPaths.hpp carries the shader-side copy). Relative
        // paths anchor to the executable directory so tests pass regardless of CWD.
        std::filesystem::path ExeRelative(const std::filesystem::path& path)
        {
            if (path.is_absolute())
                return path;
#ifdef _WIN32
            wchar_t modulePath[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
                return std::filesystem::path(modulePath).parent_path() / path;
#endif
            return path;
        }

        // Read all bytes from a file. Returns empty on any failure (kept
        // local -- not exported).
        // A legitimately empty file is treated as missing and callers memoize
        // it as a failure -- empty assets are unsupported. (This mirrored
        // ShaderLibrary::ReadFileBytes, which was deleted at ABI v15.)
        std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return {};
            const std::streamsize size = file.tellg();
            if (size <= 0)
                return {};
            file.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes((size_t)size);
            if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
                return {};
            return bytes;
        }

        // Cache key: normalised absolute path string so that relative and
        // absolute references to the same file share one entry.
        std::string CacheKey(const std::filesystem::path& resolved)
        {
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(resolved, ec);
            if (ec)
                return resolved.string();
            return canon.string();
        }

        using BytesPtr     = std::shared_ptr<const std::vector<uint8_t>>;
        using JsonPtr      = std::shared_ptr<const nlohmann::json>;
        using PixelDataPtr = std::shared_ptr<PixelData>;

        class AssetsImpl final : public Assets
        {
        public:
            explicit AssetsImpl(const AssetsDesc& desc)
                : m_byteBudget(desc.byteBudget)
            {
            }

            void SetContentRoot(const std::filesystem::path& root) override
            {
                m_contentRoot = root;
            }

            void SetAssetResolver(AssetResolver resolver) override
            {
                m_resolver = std::move(resolver);
                // A new resolver (project open / registry rescan) may know ids that
                // previously failed -- drop the warn-once memos for a clean retry.
                m_idFailures.clear();
                // KEY OWNERSHIP: "assets.unresolved" -- this producer's OWN key,
                // distinct from AssetRegistry::ScanContent's "assets" key
                // (AssetRegistry.cpp) -- ResolveId's warn-once memo is not a
                // whole-tree walk, so it must never touch "assets". A retry may
                // resolve ids that previously failed, so retract every row rather
                // than leave stale "unresolved" diagnostics for ids that are about
                // to be re-checked.
                m_unresolvedDiagnosticIds.clear();
                Diagnostics::Clear("assets.unresolved");
            }

            // THE ANCHORING CONVENTION, for all four id-shaped overloads below:
            // a path that came OUT of the installed AssetResolver is already
            // LOAD-READY and must go straight to the resolved-path workers --
            // never back through ResolveAssetPath. The registry turned the Guid
            // into a mount path and the MountTable joined it onto THAT MOUNT's
            // own root (game:// = <project>/Content, diag:// =
            // <project>/Saved/Diagnostics, plugin/<x>:// = <plugin>/Content), so
            // there is nothing left for this facade to anchor. Re-anchoring it
            // under m_contentRoot is not merely redundant -- for a relative
            // project root it produces <root>/Content/<root>/Content/... (the
            // desk-observed "failed to load ReferenceProject\Content\
            // ReferenceProject\Content\textures\uv_marker.png"), and for the
            // diag://+plugin:// mounts it is wrong even when the root is
            // absolute, because those mounts do not live under Content/ at all.
            // Every OTHER consumer of the same resolver -- SpriteCache,
            // SpriteMaterialCache, PostChainCache (via SceneRenderResolver),
            // ProjectBoot's boot-scene lookup, the editor, the NRI graph
            // vehicle's own SetAssetResolver -- opens the returned path verbatim
            // with an ifstream, so verbatim IS the convention and this facade was
            // the sole violator of it. See ResolveAssetPath for the other half of
            // the rule (who DOES anchor).
            const PixelData* PixelsFor(const Guid& id) override
            {
                const auto p = ResolveId(AssetId::FromGuid(id));
                return p ? PixelsForResolved(*p, CacheKey(*p)) : nullptr;
            }

            BytesPtr GetBytes(const AssetId& id) override
            {
                const auto p = ResolveId(id);
                return p ? BytesForResolved(*p) : nullptr;
            }

            JsonPtr GetJson(const AssetId& id) override
            {
                const auto p = ResolveId(id);
                return p ? JsonForResolved(*p) : nullptr;
            }

            // The PATH overloads: a caller-supplied LOOSE relative path is the
            // OTHER half of the convention -- the half the content root does
            // anchor. Both routes converge on the same *Resolved workers below,
            // so a file reached by Guid and the same file reached by path still
            // share ONE cache entry (CacheKey canonicalises both).
            BytesPtr GetBytes(const std::filesystem::path& path) override
            {
                return BytesForResolved(ResolveAssetPath(path));
            }

            JsonPtr GetJson(const std::filesystem::path& path) override
            {
                return JsonForResolved(ResolveAssetPath(path));
            }

            AssetStats Stats() const override
            {
                AssetStats s;
                s.totalBytes = TotalBytes();
                s.count = (uint32_t)(m_bytes.Count() +
                                     m_json.Count() +
                                     m_pixels.Count());
                return s;
            }

        private:
            // The loaders, keyed on an ALREADY-RESOLVED path -- whichever of the
            // two routes above produced it. They never anchor anything
            // themselves: the anchoring decision is made exactly once, by the
            // caller, which is what makes the id route's "already load-ready"
            // contract structural instead of incidental.
            // TextureForResolved -- the sRGB upload GetTexture rode -- is
            // deleted at ABI v15 with GetTexture itself. It had reached its
            // `m_device->createTexture` line exactly never since NRI Phase 5a,
            // Task 9: that task removed Runtime::SetRenderResources, which was
            // SetDevice's last production caller, so `if (!m_device)` short-
            // circuited every call. Its decode half survives untouched as
            // PixelsForResolved below, which is what it already delegated to.

            BytesPtr BytesForResolved(const std::filesystem::path& resolved)
            {
                const std::string key = CacheKey(resolved);

                if (m_bytes.Has(key))
                {
                    if (m_bytes.IsFailure(key))
                        return nullptr;
                    return m_bytes.Get(key);
                }

                auto raw = ReadFileBytes(resolved);
                if (raw.empty())
                {
                    ARC_WARN("Assets: file not found or empty: {}",
                             resolved.string());
                    m_bytes.PutFailure(key);
                    return nullptr;
                }

                const uint64_t sz = (uint64_t)raw.size();
                auto ptr = std::make_shared<const std::vector<uint8_t>>(
                    std::move(raw));
                m_bytes.Put(key, ptr, sz);
                EnforceBudget();
                return ptr;
            }

            JsonPtr JsonForResolved(const std::filesystem::path& resolved)
            {
                const std::string key = CacheKey(resolved);

                if (m_json.Has(key))
                {
                    if (m_json.IsFailure(key))
                        return nullptr;
                    return m_json.Get(key);
                }

                auto raw = ReadFileBytes(resolved);
                if (raw.empty())
                {
                    ARC_WARN("Assets: JSON file not found or empty: {}",
                             resolved.string());
                    m_json.PutFailure(key);
                    return nullptr;
                }

                auto doc = nlohmann::json::parse(
                    raw.begin(), raw.end(),
                    /*cb=*/nullptr,
                    /*allow_exceptions=*/false);
                if (doc.is_discarded())
                {
                    ARC_WARN("Assets: JSON parse failed: {}",
                             resolved.string());
                    m_json.PutFailure(key);
                    return nullptr;
                }

                const uint64_t sz = (uint64_t)raw.size();
                auto ptr = std::make_shared<const nlohmann::json>(
                    std::move(doc));
                m_json.Put(key, ptr, sz);
                EnforceBudget();
                return ptr;
            }

            // Decode-once pixel supply shared by PixelsFor(Guid) and
            // GetTexture (both overloads): `resolved`/`key` are the SAME
            // already-resolved path + CacheKey the texture cache uses, so a
            // Guid's pixels and its eventual GPU upload -- however either is
            // reached -- share ONE decode and ONE cache entry.
            const PixelData* PixelsForResolved(const std::filesystem::path& resolved,
                                               const std::string& key)
            {
                if (m_pixels.Has(key))
                {
                    if (m_pixels.IsFailure(key))
                        return nullptr;
                    return m_pixels.Get(key).get();
                }

                auto pixels = std::make_shared<PixelData>();
                if (!LoadPngRgba(resolved, pixels->width, pixels->height, pixels->rgba))
                {
                    // LoadPngRgba already WARN-logged the specific reason
                    // (missing file, corrupt data, non-positive dims).
                    m_pixels.PutFailure(key);
                    return nullptr;
                }

                const uint64_t bytes = (uint64_t)pixels->width * pixels->height * 4;
                const PixelData* raw = pixels.get();
                m_pixels.Put(key, pixels, bytes);
                // Pin the fresh entry for the duration of the sweep below:
                // unlike GetTexture/GetBytes/GetJson (which return an owning
                // handle/shared_ptr that survives its own cache eviction),
                // PixelsFor returns a bare pointer -- if EnforceBudget evicted
                // THIS entry before we return (the "bigger than the whole
                // budget" edge case other caches tolerate), `raw` would dangle
                // the instant this function returns. Released immediately
                // after: on every LATER call this is an ordinary, evictable
                // LRU citizen like any other entry.
                m_pixels.Acquire(key);
                EnforceBudget();
                m_pixels.Release(key);
                return raw;
            }

            uint64_t TotalBytes() const
            {
                return m_bytes.TotalBytes() +
                       m_json.TotalBytes() +
                       m_pixels.TotalBytes();
            }

            // Budget sweep, run after every insert: evict the globally
            // least-recently-used entry -- across ALL THREE caches (it was
            // four until ABI v15 deleted the texture cache with GetTexture),
            // comparable via the shared recency clock -- until the total is back
            // under budget. Pinned (refcounted) entries are never offered by
            // LeastRecentEvictable and Evict refuses them; memoized failures
            // are ~zero cost and skipped (evicting them frees nothing and
            // destroys their do-not-retry memo). The budget is strict, so an
            // entry larger than the whole budget is swept right back out --
            // the caller keeps its handle (shared ownership), the cache just
            // never settles above budget. byteBudget == 0 disables the sweep.
            void EnforceBudget()
            {
                if (m_byteBudget == 0)
                    return;
                while (TotalBytes() > m_byteBudget)
                {
                    std::string key, candKey;
                    uint64_t used = UINT64_MAX, candUsed = 0;
                    int which = -1;
                    if (m_bytes.LeastRecentEvictable(candKey, candUsed) &&
                        candUsed < used)
                    {
                        key = candKey; used = candUsed; which = 0;
                    }
                    if (m_json.LeastRecentEvictable(candKey, candUsed) &&
                        candUsed < used)
                    {
                        key = candKey; used = candUsed; which = 1;
                    }
                    if (m_pixels.LeastRecentEvictable(candKey, candUsed) &&
                        candUsed < used)
                    {
                        key = candKey; used = candUsed; which = 2;
                    }

                    bool evicted = false;
                    switch (which)
                    {
                    case 0: evicted = m_bytes.Evict(key); break;
                    case 1: evicted = m_json.Evict(key); break;
                    case 2: evicted = m_pixels.Evict(key); break;
                    default: break;
                    }
                    if (!evicted)
                        break;   // nothing evictable left (pinned/failures only)
                }
            }

            // Content-root-aware resolve, shadowing the free ExeRelative helper:
            // absolute paths pass through; a set content root anchors relatives
            // under it; otherwise the legacy exe-relative anchor.
            //
            // WHO ANCHORS: this function, and ONLY for a path the CALLER handed
            // us (the three path overloads). It is NOT applied to anything the
            // AssetResolver produced -- those are already anchored at their own
            // mount's root. One anchor per path, decided at the entry point.
            std::filesystem::path ResolveAssetPath(const std::filesystem::path& path) const
            {
                if (path.is_absolute())
                    return path;
                if (!m_contentRoot.empty())
                    return m_contentRoot / path;
                return ExeRelative(path);
            }

            // AssetId -> physical file through the installed resolver. Unresolved ids
            // warn ONCE (per id, until a new resolver is installed) -- a per-frame
            // caller polling a dead reference must not storm the log.
            //
            // WHO DOES NOT ANCHOR: this one. What comes back is LOAD-READY and is
            // handed to the *Resolved workers verbatim -- never through
            // ResolveAssetPath. A relative result (the host opened the project by
            // a relative path, e.g. `--project ReferenceProject`) is relative to
            // the process CWD, which is exactly how every other consumer of this
            // same resolver opens it (a plain ifstream), so the two agree by
            // construction. Pinned by AssetsTest.cpp's "registry-resolved ids are
            // load-ready" case (a real Project opened by a RELATIVE path, with a
            // decoy planted at the doubled location).
            std::optional<std::filesystem::path> ResolveId(const AssetId& id)
            {
                if (!id.IsValid())
                {
                    if (m_idFailures.insert(Guid::Nil()).second)
                        ARC_WARN("Assets: load requested for an invalid (nil) AssetId");
                    return std::nullopt;
                }
                if (!m_resolver)
                {
                    if (m_idFailures.insert(id.Value()).second)
                        ARC_WARN("Assets: no asset resolver installed (open a project first); "
                                 "id {}", id.Value().ToString());
                    return std::nullopt;
                }
                auto p = m_resolver(id);
                if (!p && m_idFailures.insert(id.Value()).second)
                {
                    ARC_WARN("Assets: unresolved asset id {}", id.Value().ToString());
                    // KEY OWNERSHIP: "assets.unresolved" -- this producer's OWN
                    // key. AssetRegistry::ScanContent owns the whole-tree-walk
                    // "assets" key (AssetRegistry.cpp); this memo is a per-id
                    // warn-once gate on the resolve path, not a walk, so it must
                    // NEVER Publish("assets", ...) -- that would let a single
                    // unresolved lookup clobber every row the last full scan
                    // found. m_idFailures.insert(...).second is true only the
                    // FIRST time this id fails, so this id is genuinely new here.
                    m_unresolvedDiagnosticIds.push_back(id.Value());
                    PublishUnresolvedDiagnostics();
                }
                return p;
            }

            // Rebuilds and republishes the FULL "assets.unresolved" set from
            // m_unresolvedDiagnosticIds. Diagnostics::Publish is a publication-
            // group REPLACE (see Diagnostics.hpp) -- publishing only the newest
            // id would retract every id a prior addition surfaced, so every
            // addition (ResolveId above) and every clear (SetAssetResolver)
            // republishes the whole accumulated set, not a delta.
            void PublishUnresolvedDiagnostics()
            {
                std::vector<Diagnostic> diags;
                diags.reserve(m_unresolvedDiagnosticIds.size());
                for (const Guid& g : m_unresolvedDiagnosticIds)
                {
                    Diagnostic d;
                    d.severity = DiagSeverity::Warning;
                    d.scope    = DiagScope::Assets;
                    d.code     = "assets.unresolved";
                    d.message  = "Unresolved asset id " + g.ToString();
                    d.locator  = DiagLocator::Asset(g);
                    diags.push_back(std::move(d));
                }
                Diagnostics::Publish("assets.unresolved", diags);
            }

            uint64_t m_byteBudget;
            std::filesystem::path m_contentRoot;   // empty => exe-relative (legacy)
            AssetResolver m_resolver;              // empty => AssetId loads fail
            std::unordered_set<Guid> m_idFailures; // warn-once memo per unresolved id
            // Diagnostic mirror of the SLICE of m_idFailures that came from an
            // actually-installed resolver failing to resolve (ResolveId's third
            // branch only) -- kept separate because m_idFailures ALSO carries
            // nil-id and no-resolver-installed failures, which are different
            // problems this task does not diagnose structurally. Order-preserving
            // vector (not a set): republished whole on every addition, so
            // insertion order is the row order the user sees.
            std::vector<Guid> m_unresolvedDiagnosticIds;

            // ONE recency clock across the three caches (declared first: the
            // caches capture its address) so the budget sweep can compare LRU
            // candidates cross-cache. See AssetCache's shared-clock ctor.
            // A fourth, AssetCache<nvrhi::TextureHandle> m_textures, went with
            // GetTexture at ABI v15 -- nothing had inserted into it since NRI
            // Phase 5a, Task 9.
            uint64_t m_lruClock = 0;
            AssetCache<BytesPtr>                 m_bytes{&m_lruClock};
            AssetCache<JsonPtr>                  m_json{&m_lruClock};
            AssetCache<PixelDataPtr>             m_pixels{&m_lruClock};
        };
    }

    std::unique_ptr<Assets> Assets::Create(const AssetsDesc& desc)
    {
        return std::make_unique<AssetsImpl>(desc);
    }

    namespace
    {
        // High-quality area-average (box) downscale of an RGBA8 image to dw x dh, in ONE
        // pass: each destination texel averages the full block of source texels it covers.
        // This is the correct anti-aliased downsample for an arbitrary ratio (unlike
        // repeated 2x halving, which only hits powers of two and leaves ImGui's single-tap
        // bilinear to finish an odd residual ratio -> aliasing). Downscale only (dw<=sw).
        void DownsampleRGBA(const unsigned char* src, int sw, int sh,
                            std::vector<unsigned char>& dst, int dw, int dh)
        {
            dst.resize((size_t)dw * dh * 4);
            for (int dy = 0; dy < dh; ++dy)
            {
                int sy0 = (int)((int64_t)dy * sh / dh);
                int sy1 = (int)((int64_t)(dy + 1) * sh / dh);
                if (sy1 <= sy0) sy1 = sy0 + 1;
                for (int dx = 0; dx < dw; ++dx)
                {
                    int sx0 = (int)((int64_t)dx * sw / dw);
                    int sx1 = (int)((int64_t)(dx + 1) * sw / dw);
                    if (sx1 <= sx0) sx1 = sx0 + 1;

                    uint32_t acc[4] = {0, 0, 0, 0};
                    uint32_t n = 0;
                    for (int sy = sy0; sy < sy1; ++sy)
                        for (int sx = sx0; sx < sx1; ++sx)
                        {
                            const unsigned char* p = src + ((size_t)sy * sw + sx) * 4;
                            acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3];
                            ++n;
                        }
                    unsigned char* d = dst.data() + ((size_t)dy * dw + dx) * 4;
                    d[0] = (unsigned char)(acc[0] / n);
                    d[1] = (unsigned char)(acc[1] / n);
                    d[2] = (unsigned char)(acc[2] / n);
                    d[3] = (unsigned char)(acc[3] / n);
                }
            }
        }
    }

    bool LoadDisplayPixels(const std::filesystem::path& path, uint32_t maxSize,
                           PixelData& out)
    {
        out = PixelData{};

        const std::filesystem::path resolved = ExeRelative(path);

        int w = 0, h = 0, comp = 0;
        unsigned char* data = stbi_load(resolved.string().c_str(), &w, &h, &comp, 4);
        if (!data || w <= 0 || h <= 0)
        {
            // ARC_WARN (not ARC_ERROR): a missing UI image must not trip the GPU test's
            // RenderErrorCount()==0 assertion (that counter tracks NVRHI validation only).
            ARC_WARN("LoadDisplayPixels: not found or decode failed: {}", resolved.string());
            if (data) stbi_image_free(data);
            return false;
        }

        // Optional area-average downscale so the larger dimension fits maxSize (aspect
        // preserved). Callers size maxSize to ~2x the on-screen draw size so
        // the UI's own bilinear minification stays clean (see LoadDisplayPixels' doc).
        if (maxSize > 0 && ((uint32_t)w > maxSize || (uint32_t)h > maxSize))
        {
            int dw, dh;
            if (w >= h) { dw = (int)maxSize; dh = (h * (int)maxSize + w / 2) / w; }
            else        { dh = (int)maxSize; dw = (w * (int)maxSize + h / 2) / h; }
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            DownsampleRGBA(data, w, h, out.rgba, dw, dh);
            w = dw; h = dh;
        }
        else
        {
            out.rgba.assign(data, data + (size_t)w * h * 4);
        }
        stbi_image_free(data);

        out.width  = (uint32_t)w;
        out.height = (uint32_t)h;
        return out.Valid();
    }

    // LoadDisplayTexture's body -- LoadDisplayPixels plus an RGBA8_UNORM
    // createTexture and a transient upload command list -- is deleted at ABI
    // v15 with its declaration. It had no callers: the editor's toolbar logo,
    // its last one, moved to LoadDisplayPixels + NriTextureCache at NRI Phase
    // 3, Task 11.

    void RepackStagingToRgba(const unsigned char* src, size_t rowPitch,
                             uint32_t width, uint32_t height, bool bgraSource,
                             std::vector<unsigned char>& out)
    {
        out.resize((size_t)width * height * 4);
        for (uint32_t y = 0; y < height; ++y)
        {
            const unsigned char* s = src + (size_t)y * rowPitch;
            unsigned char* d = out.data() + (size_t)y * width * 4;
            for (uint32_t x = 0; x < width; ++x)
            {
                const unsigned char* p = s + (size_t)x * 4;
                if (bgraSource)
                {
                    d[x * 4 + 0] = p[2];
                    d[x * 4 + 1] = p[1];
                    d[x * 4 + 2] = p[0];
                }
                else
                {
                    d[x * 4 + 0] = p[0];
                    d[x * 4 + 1] = p[1];
                    d[x * 4 + 2] = p[2];
                }
                // Opaque ALWAYS -- see the header: the render target's alpha
                // holds coverage math, not a statement about the picture.
                d[x * 4 + 3] = 255;
            }
        }
    }

    // ReadTexturePixels' body -- the PickBuffer idiom (staging copy, execute,
    // waitForIdle, map) feeding RepackStagingToRgba -- is deleted at ABI v15
    // with its declaration. Its only caller was SaveTexturePng, deleted with
    // it; the graph path gets the same bytes from
    // NriGraphContext::ReadCapture.

    bool WriteThumbnailPngRgba(const std::filesystem::path& path,
                               std::uint32_t width, std::uint32_t height,
                               std::vector<unsigned char> rgba, uint32_t maxWidth)
    {
        if (width == 0 || height == 0 ||
            rgba.size() < (std::size_t)width * height * 4)
        {
            ARC_WARN("WriteThumbnailPngRgba: {}x{} does not describe {} bytes: {}",
                     width, height, rgba.size(), path.generic_string());
            return false;
        }

        // OPAQUE, before the downscale so the box filter cannot average a
        // transparent texel back in: a screenshot is a picture of the screen,
        // and whatever coverage math left in the source's alpha channel must
        // not punch holes in it. Byte-identical to RepackStagingToRgba's rule,
        // which is why the NVRHI arm (ReadTexturePixels -> SaveTexturePng,
        // both deleted at ABI v15) could route through here without changing
        // what it wrote.
        for (std::size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 0xFF;

        if (maxWidth > 0 && width > maxWidth)
        {
            // Width-capped (not larger-dimension like the loader): the
            // consumer is a fixed-width thumbnail tile, and the source is a
            // viewport whose aspect the user chose.
            const std::uint32_t dw = maxWidth;
            std::uint32_t dh = (height * dw + width / 2) / width;
            if (dh < 1) dh = 1;
            std::vector<unsigned char> scaled;
            DownsampleRGBA(rgba.data(), (int)width, (int)height, scaled, (int)dw, (int)dh);
            rgba = std::move(scaled);
            width = dw;
            height = dh;
        }

        return WritePngRgba(path, width, height, rgba.data());
    }

    // SaveTexturePng -- ReadTexturePixels joined to WriteThumbnailPngRgba --
    // is deleted at ABI v15 with both its declaration and its GPU half. It had
    // no callers: the editor's cover-thumbnail write goes straight to
    // WriteThumbnailPngRgba from the graph capture (EditorApp.cpp).

    bool LoadPngRgba(const std::filesystem::path& path,
                     std::uint32_t& width, std::uint32_t& height,
                     std::vector<unsigned char>& rgba)
    {
        width = height = 0;
        rgba.clear();
        int w = 0, h = 0, comp = 0;
        unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
        if (!data || w <= 0 || h <= 0)
        {
            if (data) stbi_image_free(data);
            ARC_WARN("LoadPngRgba: failed to load {}", path.string());
            return false;
        }
        width  = static_cast<std::uint32_t>(w);
        height = static_cast<std::uint32_t>(h);
        rgba.assign(data, data + (static_cast<std::size_t>(w) * h * 4));
        stbi_image_free(data);
        return true;
    }

    bool WritePngRgba(const std::filesystem::path& path,
                      std::uint32_t width, std::uint32_t height,
                      const unsigned char* rgba)
    {
        if (!rgba || width == 0 || height == 0)
        {
            ARC_WARN("WritePngRgba: nothing to write for {}", path.string());
            return false;
        }
        std::error_code ec;
        if (const auto parent = path.parent_path(); !parent.empty())
            std::filesystem::create_directories(parent, ec);   // best-effort
        if (!stbi_write_png(path.string().c_str(),
                            static_cast<int>(width), static_cast<int>(height), 4,
                            rgba, static_cast<int>(width) * 4))
        {
            ARC_WARN("WritePngRgba: write failed: {}", path.string());
            return false;
        }
        return true;
    }
}
