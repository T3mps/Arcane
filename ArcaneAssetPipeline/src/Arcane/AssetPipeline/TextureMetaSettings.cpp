#include "Arcane/AssetPipeline/TextureMetaSettings.hpp"

#include <cstdint>
#include <string>

namespace Arcane::AssetPipeline
{
    namespace
    {
        TextureMetaSettings::Format FormatFromString(const std::string& s, TextureMetaSettings::Format fallback)
        {
            if (s == "Auto")  return TextureMetaSettings::Format::Auto;
            if (s == "Bc7")   return TextureMetaSettings::Format::Bc7;
            if (s == "Rgba8") return TextureMetaSettings::Format::Rgba8;
            return fallback;   // unrecognised string -- keep the default rather than throw
        }

        const char* FormatToString(TextureMetaSettings::Format f)
        {
            switch (f)
            {
            case TextureMetaSettings::Format::Bc7:   return "Bc7";
            case TextureMetaSettings::Format::Rgba8: return "Rgba8";
            case TextureMetaSettings::Format::Auto:  return "Auto";
            }
            return "Auto";
        }
    }

    TextureMetaSettings TextureMetaSettings::FromMetaJson(const nlohmann::json& j)
    {
        TextureMetaSettings settings{};   // every untouched field keeps its struct default

        if (j.contains("format") && j["format"].is_string())
            settings.format = FormatFromString(j["format"].get<std::string>(), settings.format);

        if (j.contains("srgb") && j["srgb"].is_boolean())
            settings.srgb = j["srgb"].get<bool>();

        if (j.contains("generateMips") && j["generateMips"].is_boolean())
            settings.generateMips = j["generateMips"].get<bool>();

        if (j.contains("maxSize") && j["maxSize"].is_number_integer())
        {
            // Read as a signed 64-bit intermediate so a negative or out-of-range value in a
            // hand-edited file can be rejected instead of silently wrapping through an unsigned
            // narrowing conversion.
            const std::int64_t v = j["maxSize"].get<std::int64_t>();
            if (v >= 0 && v <= static_cast<std::int64_t>(UINT32_MAX))
                settings.maxSize = static_cast<std::uint32_t>(v);
        }

        return settings;
    }

    nlohmann::json TextureMetaSettings::ToMetaJson() const
    {
        nlohmann::json j;
        j["format"] = FormatToString(format);
        j["srgb"] = srgb;
        j["generateMips"] = generateMips;
        j["maxSize"] = maxSize;
        return j;
    }
}
