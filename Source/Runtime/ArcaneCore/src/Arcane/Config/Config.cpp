#include <Arcane/Config/Config.hpp>

#include <Arcane/Base/Log.hpp>

#include <fstream>
#include <system_error>
#include <utility>

namespace Arcane
{
    void Config::DeepMerge(nlohmann::json& dst, const nlohmann::json& src)
    {
        if (dst.is_object() && src.is_object())
        {
            for (auto it = src.begin(); it != src.end(); ++it)
                DeepMerge(dst[it.key()], it.value());
        }
        else
        {
            dst = src;   // scalars, arrays, and type changes: src replaces dst wholesale
        }
    }

    void Config::LayerDir(const std::filesystem::path& dir)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec))
            return;

        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
                continue;

            std::ifstream in(entry.path(), std::ios::binary);
            if (!in)
            {
                ARC_WARN("Config: cannot read '{}'", entry.path().generic_string());
                continue;
            }
            auto doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
            if (doc.is_discarded())
            {
                ARC_WARN("Config: parse failed '{}'", entry.path().generic_string());
                continue;
            }

            nlohmann::json& cat = m_categories[entry.path().stem().string()];
            if (cat.is_null())
                cat = std::move(doc);      // first layer for this stem
            else
                DeepMerge(cat, doc);       // later layer wins per key
        }
    }

    void Config::LoadEngineDefaults(const std::filesystem::path& engineConfigDir)
    {
        m_categories.clear();
        LayerDir(engineConfigDir);
    }

    void Config::LayerProject(const std::filesystem::path& projectConfigDir,
                              const std::filesystem::path& userConfigDir)
    {
        LayerDir(projectConfigDir);
        LayerDir(userConfigDir);
    }

    const nlohmann::json& Config::Category(std::string_view name) const
    {
        static const nlohmann::json kEmpty = nlohmann::json::object();
        auto it = m_categories.find(std::string(name));
        return it == m_categories.end() ? kEmpty : it->second;
    }

    bool Config::HasCategory(std::string_view name) const
    {
        return m_categories.find(std::string(name)) != m_categories.end();
    }
}
