#include <Arcane/Project/AppSplashSettings.hpp>

#include <Arcane/Config/CVarFormat.hpp>   // the one sRGB <-> linear codec ("#RRGGBBAA")

#include <cstdio>
#include <string>

namespace Arcane
{
    // Both helpers go through the cvar file codec, so a colour built here, a
    // colour read from Config/app.json and the splash brush agree byte for
    // byte (CVarColorFromHex(CVarColorToHex(byte)) is exact for every byte).
    CVarColor ColorFromSrgb8(std::uint8_t r, std::uint8_t g, std::uint8_t b)
    {
        char hex[8];
        std::snprintf(hex, sizeof hex, "#%02X%02X%02X", static_cast<unsigned>(r), static_cast<unsigned>(g),
                      static_cast<unsigned>(b));
        return CVarColorFromHex(hex).value_or(CVarColor{});
    }

    std::uint32_t ToSrgb8(const CVarColor& color)
    {
        const std::string hex = CVarColorToHex(color);   // "#RRGGBBAA"
        return static_cast<std::uint32_t>(std::stoul(hex.substr(1, 6), nullptr, 16));
    }
}

ARC_SETTINGS(Arcane::AppSplashSettings);
