#include <Arcane/Render/ShaderLibrary.hpp>

#include <Arcane/Base/Log.hpp>

#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

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

            // Derive the HLSL entry-point name from the artifact stem.
            // Convention (from compile-shaders.bat): <shader>_vs -> vs_main,
            // <shader>_ps -> ps_main, <shader>_cs -> cs_main.
            // Falls back to "main" so DXIL artifacts (which embed the real
            // name) still work -- the D3D12 backend ignores entryName for DXIL
            // (DXIL encodes it). Vulkan uses it for SPIR-V OpEntryPoint lookup.
            static std::string EntryNameFromStem(std::string_view stem)
            {
                if (stem.size() >= 3)
                {
                    const auto suffix = stem.substr(stem.size() - 3);
                    if (suffix == "_vs") return "vs_main";
                    if (suffix == "_ps") return "ps_main";
                    if (suffix == "_cs") return "cs_main";
                }
                ARC_WARN("Shader '{}' has no _vs/_ps/_cs suffix; entry name "
                         "falls back to 'main' (SPIR-V needs an exact "
                         "OpEntryPoint match -- check compile-shaders.bat)",
                         stem);
                return "main";
            }

            bool LoadEntry(Entry& entry)
            {
                std::error_code ec;
                const auto sizeBefore =
                    std::filesystem::file_size(entry.path, ec);
                if (ec)
                {
                    ARC_ERROR("Shader artifact missing/unreadable: {}",
                              entry.path.string());
                    return false;
                }

                const std::vector<char> bytes = ReadFileBytes(entry.path);

                const auto sizeAfter =
                    std::filesystem::file_size(entry.path, ec);
                if (ec || bytes.empty() || bytes.size() != sizeBefore ||
                    sizeAfter != sizeBefore)
                {
                    // Likely a torn read: the compiler is still writing.
                    // Keep the previous handle; Poll retries next cycle
                    // (the stamp was not refreshed).
                    ARC_WARN("Shader artifact changed mid-read, retrying: {}",
                             entry.path.string());
                    return false;
                }

                // entryName must match the HLSL function name in SPIR-V
                // (Vulkan requires an exact OpEntryPoint match). For DXIL the
                // D3D12 backend ignores this field, but set it for symmetry.
                const std::string stem = entry.path.stem().string();
                auto desc = nvrhi::ShaderDesc()
                    .setShaderType(entry.type)
                    .setEntryName(EntryNameFromStem(stem))
                    .setDebugName(entry.path.filename().string());
                entry.handle =
                    m_device->createShader(desc, bytes.data(), bytes.size());
                if (!entry.handle)
                {
                    ARC_ERROR("createShader failed: {}", entry.path.string());
                    return false;
                }
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

        if (dir.is_relative())
        {
            // Relative dirs anchor to the executable, not the CWD: the
            // build copies artifacts next to consumer exes, and tests must
            // pass regardless of where they are launched from.
#ifdef _WIN32
            wchar_t modulePath[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) != 0)
                dir = std::filesystem::path(modulePath).parent_path() / dir;
#endif
        }
        dir /= (backend == GraphicsBackend::Vulkan) ? "spirv" : "dxil";

        if (!std::filesystem::is_directory(dir))
        {
            ARC_ERROR("Shader directory not found: {}", dir.string());
            return nullptr;
        }
        return std::make_unique<ShaderLibraryImpl>(device, dir);
    }
}
