#include <Arcane/Project/ProjectManifest.hpp>

#include <Arcane/Base/Log.hpp>   // ARC_WARN (defined at Log.hpp:40)

#include <Json.hpp>

#include <fstream>
#include <sstream>

namespace Arcane
{
    std::optional<ProjectManifest> ProjectManifest::FromJson(const nlohmann::json& doc)
    try
    {
        if (!doc.is_object())
            return std::nullopt;

        ProjectManifest m;

        // Required: formatVersion (> 0), name (non-empty), engine.abi (int).
        if (!doc.contains("formatVersion") || !doc["formatVersion"].is_number_integer())
            return std::nullopt;
        m.formatVersion = doc["formatVersion"].get<int>();
        if (m.formatVersion <= 0)
            return std::nullopt;

        if (!doc.contains("name") || !doc["name"].is_string())
            return std::nullopt;
        m.name = doc["name"].get<std::string>();
        if (m.name.empty())
            return std::nullopt;

        if (!doc.contains("engine") || !doc["engine"].is_object()
            || !doc["engine"].contains("abi") || !doc["engine"]["abi"].is_number_integer())
            return std::nullopt;
        m.engineAbi = doc["engine"]["abi"].get<int>();

        // Optional. doc.value(key, default) throws nlohmann::json::type_error if the key
        // exists with the wrong type -- caught by the function-try-block below so a
        // type-mismatched optional field yields nullopt rather than propagating an
        // exception (same contract as the required-field guards above).
        m.description = doc.value("description", std::string{});
        m.gameModule  = doc.value("gameModule", std::string{});
        m.bootScene   = doc.value("bootScene", std::string{});
        m.guid        = doc.value("guid", std::string{});

        if (doc.contains("plugins") && doc["plugins"].is_array())
        {
            for (const auto& p : doc["plugins"])
            {
                if (!p.is_object() || !p.contains("name") || !p["name"].is_string())
                    continue;
                PluginRef ref;
                ref.name    = p["name"].get<std::string>();
                ref.enabled = p.value("enabled", true);
                m.plugins.push_back(std::move(ref));
            }
        }

        return m;
    }
    catch (const nlohmann::json::exception&)
    {
        return std::nullopt;
    }

    std::optional<ProjectManifest> ProjectManifest::LoadFile(const std::filesystem::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            ARC_WARN("ProjectManifest: cannot open '{}'", file.generic_string());
            return std::nullopt;
        }
        std::stringstream ss;
        ss << in.rdbuf();

        nlohmann::json doc;
        try
        {
            doc = nlohmann::json::parse(ss.str());
        }
        catch (const std::exception& e)
        {
            ARC_WARN("ProjectManifest: parse failed for '{}': {}", file.generic_string(), e.what());
            return std::nullopt;
        }
        auto m = FromJson(doc);
        if (!m)
            ARC_WARN("ProjectManifest: schema invalid in '{}'", file.generic_string());
        return m;
    }
}
