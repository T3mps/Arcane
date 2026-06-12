#include <Arcane/Render/ShaderLibrary.hpp>

#include <Arcane/Base/Log.hpp>

#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace Arcane
{
    namespace
    {
        std::vector<char> ReadFileBytes(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return {};
            const std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::vector<char> bytes((size_t)size);
            if (!file.read(bytes.data(), size))
                return {};
            return bytes;
        }

        class ShaderLibraryImpl final : public ShaderLibrary
        {
        public:
            ShaderLibraryImpl(nvrhi::IDevice* device,
                              std::filesystem::path flavorDir)
                : m_device(device), m_flavorDir(std::move(flavorDir))
            {
            }

            nvrhi::ShaderHandle Get(std::string_view name,
                                    nvrhi::ShaderType type) override
            {
                const std::string key(name);
                auto it = m_entries.find(key);
                if (it != m_entries.end())
                    return it->second.handle;

                Entry entry;
                entry.path = m_flavorDir / (key + ".bin");
                entry.type = type;
                if (!LoadEntry(entry))
                    return nullptr;
                m_entries.emplace(key, entry);
                return entry.handle;
            }

            bool Poll() override
            {
                bool reloaded = false;
                for (auto& [name, entry] : m_entries)
                {
                    std::error_code ec;
                    const auto stamp =
                        std::filesystem::last_write_time(entry.path, ec);
                    if (ec || stamp == entry.stamp)
                        continue;
                    Entry fresh = entry;
                    if (LoadEntry(fresh))
                    {
                        entry = fresh;
                        reloaded = true;
                        ARC_INFO("Shader reloaded: {}", name);
                    }
                }
                if (reloaded)
                    ++m_generation;
                return reloaded;
            }

            uint64_t Generation() const override { return m_generation; }

        private:
            struct Entry
            {
                std::filesystem::path path;
                std::filesystem::file_time_type stamp{};
                nvrhi::ShaderType type = nvrhi::ShaderType::None;
                nvrhi::ShaderHandle handle;
            };

            bool LoadEntry(Entry& entry)
            {
                const std::vector<char> bytes = ReadFileBytes(entry.path);
                if (bytes.empty())
                {
                    ARC_ERROR("Shader artifact missing/unreadable: {}",
                              entry.path.string());
                    return false;
                }
                auto desc = nvrhi::ShaderDesc()
                    .setShaderType(entry.type)
                    .setDebugName(entry.path.filename().string());
                entry.handle =
                    m_device->createShader(desc, bytes.data(), bytes.size());
                if (!entry.handle)
                {
                    ARC_ERROR("createShader failed: {}", entry.path.string());
                    return false;
                }
                std::error_code ec;
                entry.stamp = std::filesystem::last_write_time(entry.path, ec);
                return true;
            }

            nvrhi::IDevice* m_device;
            std::filesystem::path m_flavorDir;
            std::unordered_map<std::string, Entry> m_entries;
            uint64_t m_generation = 1;
        };
    }

    std::unique_ptr<ShaderLibrary> ShaderLibrary::Create(
        nvrhi::IDevice* device, GraphicsBackend backend,
        const std::filesystem::path& shaderDir)
    {
        std::filesystem::path dir = shaderDir;
        if (const char* overrideDir = std::getenv("ARCANE_SHADER_DIR"))
            dir = overrideDir;
        dir /= (backend == GraphicsBackend::Vulkan) ? "spirv" : "dxil";

        if (!std::filesystem::is_directory(dir))
        {
            ARC_ERROR("Shader directory not found: {}", dir.string());
            return nullptr;
        }
        return std::make_unique<ShaderLibraryImpl>(device, dir);
    }
}
