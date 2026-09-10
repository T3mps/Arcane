#include "Arcane/AssetPipeline/MeshMetaSettings.hpp"

#include <cstdint>

namespace Arcane::AssetPipeline
{
    MeshMetaSettings MeshMetaSettings::FromMetaJson(const nlohmann::json& j)
    {
        MeshMetaSettings settings{};   // every untouched field keeps its struct default

        // is_number_unsigned, not is_number -- gates out both a wrong-shape value and a
        // hand-edited negative number in one check: get<uint32_t>() on a JSON -3 would
        // silently wrap to 4294967293 rather than falling back the way every other
        // malformed field here does (same rule MeshAsset.cpp's own readUint uses).
        if (j.contains("settingsVersion") && j["settingsVersion"].is_number_unsigned())
            settings.settingsVersion = j["settingsVersion"].get<std::uint32_t>();

        return settings;
    }

    nlohmann::json MeshMetaSettings::ToMetaJson() const
    {
        nlohmann::json j;
        j["settingsVersion"] = settingsVersion;
        return j;
    }
}
