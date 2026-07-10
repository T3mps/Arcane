#include <Arcane/Assets/Assets.hpp>

#include <Arcane/Assets/AssetCache.hpp>
#include <Arcane/Base/Log.hpp>

#include <Json.hpp>
#include <stb_image.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Arcane
{
    namespace
    {
        // Resolve a path exe-relative when relative -- mirrors the pattern in
        // ShaderLibrary.cpp for consistency. Relative paths anchor to the
        // executable directory so tests pass regardless of CWD.
        std::filesystem::path ResolveAssetPath(const std::filesystem::path& path)
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

            nvrhi::IDevice* m_device;
            uint64_t m_byteBudget;

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
}
