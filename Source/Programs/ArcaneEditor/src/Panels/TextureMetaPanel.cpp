#include "Panels/TextureMetaPanel.hpp"

#include <Arcane/Base/Log.hpp>

#include <Json.hpp>

#include <fstream>
#include <utility>

namespace Arcane::Editor
{
    Arcane::AssetPipeline::TextureMetaSettings ReadTextureMetaSettingsDisplay(
        const std::filesystem::path& metaPath, const Arcane::AssetPipeline::TextureMetaSettings& defaults,
        Arcane::AssetPipeline::TextureMetaSettings::FieldsSet* set)
    {
        if (set)
            *set = {};
        std::ifstream in(metaPath, std::ios::binary);
        if (!in)
            return defaults;
        const nlohmann::json doc = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
        if (!doc.is_object())
            return defaults;
        nlohmann::json textureBlock = nlohmann::json::object();
        if (const auto it = doc.find("texture"); it != doc.end() && it->is_object())
            textureBlock = *it;
        return Arcane::AssetPipeline::TextureMetaSettings::FromMetaJson(textureBlock, defaults, set);
    }

    void WriteTextureMetaSettingsMerged(const std::filesystem::path& metaPath,
                                        const Arcane::AssetPipeline::TextureMetaSettings& settings,
                                        const Arcane::AssetPipeline::TextureMetaSettings::FieldsSet* only)
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
        nlohmann::json block = settings.ToMetaJson();
        if (only)
        {
            if (!only->format)       block.erase("format");
            if (!only->srgb)         block.erase("srgb");
            if (!only->generateMips) block.erase("generateMips");
            if (!only->maxSize)      block.erase("maxSize");
        }
        doc["texture"] = std::move(block);
        // Plain truncate-overwrite, no temp+rename -- a DECISION, not an
        // omission (task-review finding): AssetRegistry.cpp's own
        // ResolveSidecarId writes its sidecar the same way, both readers here
        // and there tolerate a torn read (a parse failure degrades to
        // defaults/nullopt, never a crash), and atomic-write hardening for
        // sidecars in general belongs to a dedicated pass across every writer
        // if one ever proves necessary -- not a one-off fix bolted onto this
        // single call site.
        std::ofstream out(metaPath, std::ios::binary);
        if (!out)
        {
            ARC_WARN("TextureMetaPanel: cannot write '{}'", metaPath.generic_string());
            return;
        }
        out << doc.dump(2) << '\n';
    }
}
