#pragma once

// log.* (settings arc S2): the engine log level. LogSettings::level is the
// former `log.level` cvar Log::Init registered, with the same name, type,
// range, help and flags (Archive|Dev). Its default is Init()'s default (info);
// every Init caller passes the default.

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Core/Api.hpp>

#include <cstdint>

namespace Arcane
{
    struct LogSettings
    {
        std::int32_t level = 2;   // spdlog::level::info == Mosaic::LogLevel::Info
    };

    ARC_REFLECT_TYPE(LogSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "log", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_FIELD(LogSettings, level)
            ARC_REFLECT_ATTR(Range, 0.0, 6.0)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Engine log level: 0 trace, 1 debug, 2 info, 3 warn, 4 error, 5 critical, 6 off. "
                                         "Gates stderr, the log file and the Console.")
    ARC_END_REFLECT_TYPE()

    // spdlog's level and Mosaic's level in EVERY module (Log::SetMosaicLevelEverywhere).
    // The two scales match: 0 trace ... 6 off.
    ARC_CORE_API void ApplyLogSettings(const LogSettings& settings);
}
