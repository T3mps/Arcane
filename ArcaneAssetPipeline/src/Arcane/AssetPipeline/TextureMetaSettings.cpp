#include "Arcane/AssetPipeline/TextureMetaSettings.hpp"

#include <Arcane/Util/Logger.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace Arcane::AssetPipeline
{
    namespace
    {
        std::string ToLowerAscii(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // I1 fix (final-review wave, 2026-09-04): case-insensitive, matching spec s4's own
        // spelling ("format (auto|bc7|rgba8)", lowercase) -- the parser used to require an
        // EXACT "Auto"/"Bc7"/"Rgba8" match, silently falling back to Auto even for the
        // spec's own documented lowercase spellings. An unrecognised string still falls
        // back to `fallback` (never throws -- this stays as tolerant as every other field
        // in this struct's FromMetaJson), but now WARNS once per parse so a hand-edited
        // typo is visible instead of silently doing nothing. ToMetaJson's own output
        // spelling is UNCHANGED by this fix (still "Auto"/"Bc7"/"Rgba8") -- round-trip
        // through this engine's own writer is unaffected either way.
        TextureMetaSettings::Format FormatFromString(const std::string& s, TextureMetaSettings::Format fallback)
        {
            const std::string lower = ToLowerAscii(s);
            if (lower == "auto")  return TextureMetaSettings::Format::Auto;
            if (lower == "bc7")   return TextureMetaSettings::Format::Bc7;
            if (lower == "rgba8") return TextureMetaSettings::Format::Rgba8;

            ::Arcane::Logger::Get("AssetPipeline")->warn(
                "TextureMetaSettings: unrecognised \"format\" value '{}' in a .meta texture "
                "block (expected auto|bc7|rgba8, case-insensitive) -- falling back to Auto", s);
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
