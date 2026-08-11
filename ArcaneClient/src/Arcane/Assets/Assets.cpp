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
        // Resolve a path exe-relative when relative -- mirrors ShaderLibrary. Relative
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

        // Read all bytes from a file. Returns empty on any failure (mirrors
        // ShaderLibrary::ReadFileBytes; kept local -- not exported).
        // A legitimately empty file is treated as missing and callers memoize
        // it as a failure -- empty assets are unsupported (matches ShaderLibrary
        // semantics).
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

        using BytesPtr = std::shared_ptr<const std::vector<uint8_t>>;
        using JsonPtr  = std::shared_ptr<const nlohmann::json>;

        class AssetsImpl final : public Assets
        {
        public:
            AssetsImpl(nvrhi::IDevice* device, const AssetsDesc& desc)
                : m_device(device), m_byteBudget(desc.byteBudget)
            {
            }

            void SetDevice(nvrhi::IDevice* device) override
            {
                if (device == m_device)
                    return;
                m_device = device;
                // Cached textures belong to the old device, and any failure
                // memoized while the device was null (see GetTexture's guard)
                // was not a real load failure -- drop both so a bound device
                // gets a clean retry. Bytes/JSON caches are device-independent.
                m_textures.Clear();
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

            nvrhi::TextureHandle GetTexture(const AssetId& id) override
            {
                const auto p = ResolveId(id);
                return p ? GetTexture(*p) : nullptr;
            }

            BytesPtr GetBytes(const AssetId& id) override
            {
                const auto p = ResolveId(id);
                return p ? GetBytes(*p) : nullptr;
            }

            JsonPtr GetJson(const AssetId& id) override
            {
                const auto p = ResolveId(id);
                return p ? GetJson(*p) : nullptr;
            }

            nvrhi::TextureHandle GetTexture(
                const std::filesystem::path& path) override
            {
                const auto resolved = ResolveAssetPath(path);
                const std::string key = CacheKey(resolved);

                if (m_textures.Has(key))
                {
                    if (m_textures.IsFailure(key))
                        return nullptr;
                    return m_textures.Get(key);
                }

                // No render device yet (headless, or GetTexture raced ahead of
                // SetDevice/SetRenderResources): degrade to null instead of
                // dereferencing a null m_device. Memoize the miss so a per-frame
                // caller does not restart a warn storm; SetDevice clears it later.
                if (!m_device)
                {
                    ARC_WARN("Assets: GetTexture before render device set");
                    m_textures.PutFailure(key);
                    return nullptr;
                }

                int w = 0, h = 0, comp = 0;
                unsigned char* data = stbi_load(
                    resolved.string().c_str(), &w, &h, &comp, 4);
                if (!data)
                {
                    // ARC_WARN chosen over ARC_ERROR: asset-not-found does NOT
                    // increment RenderErrorCount() (which is fed only by NVRHI
                    // validation callbacks). Using ARC_WARN keeps the
                    // RenderErrorCount() == 0 assertion clean in the GPU test.
                    ARC_WARN("Assets: texture not found or decode failed: {}",
                             resolved.string());
                    m_textures.PutFailure(key);
                    return nullptr;
                }

                if (w <= 0 || h <= 0)
                {
                    ARC_WARN("Assets: texture has non-positive dimensions ({}x{}): {}",
                             w, h, resolved.string());
                    stbi_image_free(data);
                    m_textures.PutFailure(key);
                    return nullptr;
                }

                const uint64_t bytes = (uint64_t)w * h * 4;
                auto texDesc = nvrhi::TextureDesc()
                    .setWidth((uint32_t)w)
                    .setHeight((uint32_t)h)
                    .setFormat(nvrhi::Format::SRGBA8_UNORM)
                    .setInitialState(nvrhi::ResourceStates::ShaderResource)
                    .setKeepInitialState(true)
                    .setDebugName(resolved.filename().string().c_str());
                nvrhi::TextureHandle tex = m_device->createTexture(texDesc);
                if (!tex)
                {
                    ARC_WARN("Assets: createTexture failed for: {}",
                             resolved.string());
                    stbi_image_free(data);
                    m_textures.PutFailure(key);
                    return nullptr;
                }

                // Upload via transient command list -- exact pattern from
                // Batcher2D::Init (white texel upload).
                {
                    nvrhi::CommandListHandle upload =
                        m_device->createCommandList();
                    upload->open();
                    upload->writeTexture(tex, 0, 0, data, (size_t)w * 4);
                    upload->close();
                    m_device->executeCommandList(upload);
                }
                stbi_image_free(data);

                m_textures.Put(key, tex, bytes);
                EnforceBudget();
                return tex;
            }

            BytesPtr GetBytes(const std::filesystem::path& path) override
            {
                const auto resolved = ResolveAssetPath(path);
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

            JsonPtr GetJson(const std::filesystem::path& path) override
            {
                const auto resolved = ResolveAssetPath(path);
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

            AssetStats Stats() const override
            {
                AssetStats s;
                s.totalBytes = TotalBytes();
                s.count = (uint32_t)(m_textures.Count() +
                                     m_bytes.Count() +
                                     m_json.Count());
                return s;
            }

        private:
            uint64_t TotalBytes() const
            {
                return m_textures.TotalBytes() +
                       m_bytes.TotalBytes() +
                       m_json.TotalBytes();
            }

            // Budget sweep, run after every insert: evict the globally
            // least-recently-used entry -- across ALL three caches, comparable
            // via the shared recency clock -- until the facade total is back
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
                    if (m_textures.LeastRecentEvictable(candKey, candUsed) &&
                        candUsed < used)
                    {
                        key = candKey; used = candUsed; which = 0;
                    }
                    if (m_bytes.LeastRecentEvictable(candKey, candUsed) &&
                        candUsed < used)
                    {
                        key = candKey; used = candUsed; which = 1;
                    }
                    if (m_json.LeastRecentEvictable(candKey, candUsed) &&
                        candUsed < used)
                    {
                        key = candKey; used = candUsed; which = 2;
                    }

                    bool evicted = false;
                    switch (which)
                    {
                    case 0: evicted = m_textures.Evict(key); break;
                    case 1: evicted = m_bytes.Evict(key); break;
                    case 2: evicted = m_json.Evict(key); break;
                    default: break;
                    }
                    if (!evicted)
                        break;   // nothing evictable left (pinned/failures only)
                }
            }

            // Content-root-aware resolve, shadowing the free ExeRelative helper for
            // the three Get* methods: absolute paths pass through; a set content root
            // anchors relatives under it; otherwise the legacy exe-relative anchor.
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
            // caller polling a dead reference must not storm the log. The resolved
            // path feeds the path loaders, whose own caches key on the file.
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

            nvrhi::IDevice* m_device;
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
            uint64_t m_lruClock = 0;
            AssetCache<nvrhi::TextureHandle>  m_textures{&m_lruClock};
            AssetCache<BytesPtr>              m_bytes{&m_lruClock};
            AssetCache<JsonPtr>               m_json{&m_lruClock};
        };
    }

    std::unique_ptr<Assets> Assets::Create(nvrhi::IDevice* device,
                                           const AssetsDesc& desc)
    {
        return std::make_unique<AssetsImpl>(device, desc);
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

    nvrhi::TextureHandle LoadDisplayTexture(nvrhi::IDevice* device,
                                            const std::filesystem::path& path,
                                            uint32_t maxSize)
    {
        if (!device)
        {
            ARC_WARN("LoadDisplayTexture: no render device");
            return nullptr;
        }

        const std::filesystem::path resolved = ExeRelative(path);

        int w = 0, h = 0, comp = 0;
        unsigned char* data = stbi_load(resolved.string().c_str(), &w, &h, &comp, 4);
        if (!data || w <= 0 || h <= 0)
        {
            // ARC_WARN (not ARC_ERROR): a missing UI image must not trip the GPU test's
            // RenderErrorCount()==0 assertion (that counter tracks NVRHI validation only).
            ARC_WARN("LoadDisplayTexture: not found or decode failed: {}", resolved.string());
            if (data) stbi_image_free(data);
            return nullptr;
        }

        // Optional area-average downscale so the larger dimension fits maxSize (aspect
        // preserved). `src`/w/h track the pixels uploaded -- either the stb buffer (no
        // downscale) or `scaled`. Callers size maxSize to ~2x the on-screen draw size so
        // the UI's own bilinear minification stays clean (see LoadDisplayTexture's doc).
        const unsigned char* src = data;
        std::vector<unsigned char> scaled;
        if (maxSize > 0 && ((uint32_t)w > maxSize || (uint32_t)h > maxSize))
        {
            int dw, dh;
            if (w >= h) { dw = (int)maxSize; dh = (h * (int)maxSize + w / 2) / w; }
            else        { dh = (int)maxSize; dw = (w * (int)maxSize + h / 2) / h; }
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            DownsampleRGBA(src, w, h, scaled, dw, dh);
            src = scaled.data();
            w = dw; h = dh;
        }

        // RGBA8_UNORM, NOT sRGB: the sampled texel goes straight to the display-referred
        // target (matches the ImGui backend's own font/texture format in ImGuiNvrhi.cpp).
        auto texDesc = nvrhi::TextureDesc()
            .setWidth((uint32_t)w)
            .setHeight((uint32_t)h)
            .setFormat(nvrhi::Format::RGBA8_UNORM)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true)
            .setDebugName(resolved.filename().string().c_str());
        nvrhi::TextureHandle tex = device->createTexture(texDesc);
        if (!tex)
        {
            ARC_WARN("LoadDisplayTexture: createTexture failed for: {}", resolved.string());
            stbi_image_free(data);
            return nullptr;
        }

        // Transient upload command list -- same pattern as Assets::GetTexture / Batcher2D.
        nvrhi::CommandListHandle upload = device->createCommandList();
        upload->open();
        upload->writeTexture(tex, 0, 0, src, (size_t)w * 4);
        upload->close();
        device->executeCommandList(upload);
        stbi_image_free(data);

        return tex;
    }

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

    bool SaveTexturePng(nvrhi::IDevice* device, nvrhi::ITexture* texture,
                        const std::filesystem::path& path, uint32_t maxWidth)
    {
        if (!device || !texture)
        {
            ARC_WARN("SaveTexturePng: no device or texture for {}", path.string());
            return false;
        }
        const nvrhi::TextureDesc& srcDesc = texture->getDesc();
        const bool bgra = srcDesc.format == nvrhi::Format::BGRA8_UNORM;
        if (!bgra && srcDesc.format != nvrhi::Format::RGBA8_UNORM)
        {
            ARC_WARN("SaveTexturePng: unsupported source format for {}", path.string());
            return false;
        }

        // The PickBuffer idiom: staging copy, execute, waitForIdle, map. One
        // synchronous stall, owned by a rare event (save/shutdown).
        auto stagingDesc = nvrhi::TextureDesc()
            .setWidth(srcDesc.width)
            .setHeight(srcDesc.height)
            .setFormat(srcDesc.format)
            .setDebugName("SaveTexturePngStaging");
        nvrhi::StagingTextureHandle staging =
            device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
        if (!staging)
        {
            ARC_WARN("SaveTexturePng: createStagingTexture failed for {}", path.string());
            return false;
        }

        nvrhi::CommandListHandle cl = device->createCommandList();
        cl->open();
        cl->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
        cl->close();
        device->executeCommandList(cl);
        device->waitForIdle();

        size_t rowPitch = 0;
        const auto* mapped = static_cast<const unsigned char*>(device->mapStagingTexture(
            staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
        if (!mapped)
        {
            ARC_WARN("SaveTexturePng: mapStagingTexture failed for {}", path.string());
            return false;
        }
        std::vector<unsigned char> rgba;
        RepackStagingToRgba(mapped, rowPitch, srcDesc.width, srcDesc.height, bgra, rgba);
        device->unmapStagingTexture(staging);
        device->runGarbageCollection();

        int w = (int)srcDesc.width, h = (int)srcDesc.height;
        const unsigned char* pixels = rgba.data();
        std::vector<unsigned char> scaled;
        if (maxWidth > 0 && (uint32_t)w > maxWidth)
        {
            // Width-capped (not larger-dimension like the loader): the
            // consumer is a fixed-width thumbnail tile, and the source is a
            // viewport whose aspect the user chose.
            int dw = (int)maxWidth;
            int dh = (h * dw + w / 2) / w;
            if (dh < 1) dh = 1;
            DownsampleRGBA(pixels, w, h, scaled, dw, dh);
            pixels = scaled.data();
            w = dw;
            h = dh;
        }

        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (!stbi_write_png(path.string().c_str(), w, h, 4, pixels, w * 4))
        {
            ARC_WARN("SaveTexturePng: write failed: {}", path.string());
            return false;
        }
        return true;
    }
}
