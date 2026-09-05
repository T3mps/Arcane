#include "Panels/TextureMetaPanel.hpp"

#include <Arcane/Base/Log.hpp>

#include <Json.hpp>

#include <fstream>

namespace Arcane::Editor
{
    Arcane::AssetPipeline::TextureMetaSettings ReadTextureMetaSettingsDisplay(
        const std::filesystem::path& metaPath)
    {
        std::ifstream in(metaPath, std::ios::binary);
        if (!in)
            return {};
        const nlohmann::json doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
        if (!doc.is_object())
            return {};
        nlohmann::json textureBlock = nlohmann::json::object();
        if (const auto it = doc.find("texture"); it != doc.end() && it->is_object())
            textureBlock = *it;
        return Arcane::AssetPipeline::TextureMetaSettings::FromMetaJson(textureBlock);
    }

    void WriteTextureMetaSettingsMerged(const std::filesystem::path& metaPath,
                                        const Arcane::AssetPipeline::TextureMetaSettings& settings)
    {
        nlohmann::json doc;
        {
            std::ifstream in(metaPath, std::ios::binary);
            if (!in)
            {
                ARC_WARN("TextureMetaPanel: cannot read '{}' -- refusing to write texture settings",
                         metaPath.generic_string());
                return;
            }
            doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
        }
        if (!doc.is_object())
        {
            ARC_WARN("TextureMetaPanel: '{}' is not a JSON object -- refusing to write texture "
                     "settings (would drop its guid)", metaPath.generic_string());
            return;
        }
        doc["texture"] = settings.ToMetaJson();
        std::ofstream out(metaPath, std::ios::binary);
        if (!out)
        {
            ARC_WARN("TextureMetaPanel: cannot write '{}'", metaPath.generic_string());
            return;
        }
        out << doc.dump(2) << '\n';
    }
}
