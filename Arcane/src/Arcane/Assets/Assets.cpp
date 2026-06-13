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
            explicit AssetsImpl(nvrhi::IDevice* device)
                : m_device(device)
            {
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
                return ptr;
            }

            AssetStats Stats() const override
            {
                AssetStats s;
                s.totalBytes = m_textures.TotalBytes() +
                               m_bytes.TotalBytes() +
                               m_json.TotalBytes();
                s.count = (uint32_t)(m_textures.Count() +
                                     m_bytes.Count() +
                                     m_json.Count());
                return s;
            }

        private:
            nvrhi::IDevice* m_device;
            AssetCache<nvrhi::TextureHandle>  m_textures;
            AssetCache<BytesPtr>              m_bytes;
            AssetCache<JsonPtr>               m_json;
        };
    }

    std::unique_ptr<Assets> Assets::Create(nvrhi::IDevice* device)
    {
        return std::make_unique<AssetsImpl>(device);
    }
}
