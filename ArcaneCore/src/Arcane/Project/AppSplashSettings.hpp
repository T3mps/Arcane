#pragma once

// Project-owned boot-splash settings. These live in Config/app.json under
// app.splash.*; .arcproj keeps only project identity and module metadata.

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Core/Api.hpp>

#include <cstdint>
#include <string>

namespace Arcane
{
    ARC_CORE_API CVarColor ColorFromSrgb8(std::uint8_t r, std::uint8_t g, std::uint8_t b);
    ARC_CORE_API std::uint32_t ToSrgb8(const CVarColor& color);

    struct AppSplashSettings
    {
        bool        enabled = true;
        std::string image;
        CVarColor   backgroundColor = ColorFromSrgb8(0x0D, 0x0D, 0x0F);
        bool        showProgress = false;
        float       minDurationSeconds = 0.0f;
        std::uint32_t width  = 480;
        std::uint32_t height = 270;
        CVarColor   textColor = ColorFromSrgb8(160, 160, 160);
    };

    ARC_REFLECT_TYPE(AppSplashSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "app.splash", SettingScope::Project, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_FIELD(AppSplashSettings, enabled)
            ARC_REFLECT_ATTR(Tooltip, "Show a boot splash while the application starts.")
        ARC_REFLECT_FIELD(AppSplashSettings, image)
            ARC_REFLECT_ATTR(Widget, "asset:texture")
            ARC_REFLECT_ATTR(Tooltip, "Texture shown by the boot splash; empty uses the engine branding.")
        ARC_REFLECT_FIELD(AppSplashSettings, backgroundColor)
            ARC_REFLECT_ATTR(Tooltip, "Linear background colour behind the boot splash image.")
        ARC_REFLECT_FIELD(AppSplashSettings, showProgress)
            ARC_REFLECT_ATTR(Tooltip, "Show startup status and progress on player boot splashes.")
        ARC_REFLECT_FIELD(AppSplashSettings, minDurationSeconds)
            ARC_REFLECT_ATTR(Range, 0.0, 10.0)
            ARC_REFLECT_ATTR(Tooltip, "Minimum time in seconds that the boot splash remains visible.")
        ARC_REFLECT_FIELD(AppSplashSettings, width)
            ARC_REFLECT_ATTR(Range, 64.0, 4096.0)
            ARC_REFLECT_ATTR(Tooltip, "Boot splash window width in pixels.")
        ARC_REFLECT_FIELD(AppSplashSettings, height)
            ARC_REFLECT_ATTR(Range, 64.0, 4096.0)
            ARC_REFLECT_ATTR(Tooltip, "Boot splash window height in pixels.")
        ARC_REFLECT_FIELD(AppSplashSettings, textColor)
            ARC_REFLECT_ATTR(Tooltip, "Linear colour of the boot splash status text.")
    ARC_END_REFLECT_TYPE()
}
