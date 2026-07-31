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

        // Splash block: leniently, exactly like plugins above -- a missing key,
        // or one present with the wrong TYPE (not an object), yields the
        // SplashConfig defaults rather than failing the manifest. Once inside a
        // well-typed object, a wrong-typed FIELD (e.g. "showProgress": "yes")
        // still throws via .value() below, same as description/gameModule/
        // plugins[].enabled above -- caught by this function's own try/catch,
        // so the whole manifest reports nullopt rather than silently defaulting
        // just that one field (consistent with every other optional field this
        // function parses).
        if (doc.contains("splash") && doc["splash"].is_object())
        {
            const auto& sp = doc["splash"];
            ProjectManifest::SplashConfig cfg;   // defaults
            cfg.enabled            = sp.value("enabled", cfg.enabled);
            cfg.image              = sp.value("image", cfg.image);
            cfg.showProgress       = sp.value("showProgress", cfg.showProgress);
            cfg.minDurationSeconds = sp.value("minDurationSeconds", cfg.minDurationSeconds);
            // backgroundColor: an array field with no precedent among the scalar
            // optionals above. Same lenient spirit as the plugins ARRAY check --
            // present but malformed (wrong type, too short) leaves the default
            // rather than failing the manifest -- but each ELEMENT must still be
            // a number to be accepted, so a partially-numeric array cannot leave
            // the default and an explicit value mixed across channels.
            if (sp.contains("backgroundColor") && sp["backgroundColor"].is_array()
                && sp["backgroundColor"].size() >= 3
                && sp["backgroundColor"][0].is_number() && sp["backgroundColor"][1].is_number()
                && sp["backgroundColor"][2].is_number())
            {
                cfg.backgroundColor[0] = sp["backgroundColor"][0].get<float>();
                cfg.backgroundColor[1] = sp["backgroundColor"][1].get<float>();
                cfg.backgroundColor[2] = sp["backgroundColor"][2].get<float>();
            }
            m.splash = cfg;
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
