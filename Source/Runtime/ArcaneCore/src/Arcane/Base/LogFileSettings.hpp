#pragma once

// log.file.* (settings arc S6-3, inventory Part 1 "Base / Diagnostics / Log"):
// the engine log FILE's retention and flush policy. Log::AttachFileSink reads
// both at every attach (boot, and each project switch's retarget);
// LogFileSettings.cpp re-applies flushLevel to an attached sink when it
// publishes. The member initializers are the pre-sweep literals.

#include <Arcane/Config/Settings.hpp>

#include <cstdint>

namespace Arcane
{
    struct LogFileSettings
    {
        std::int32_t keepCount = 5;    // rotated files kept beside the live one
        std::int32_t flushLevel = 3;   // spdlog level: 0 trace .. 3 warn .. 6 off
    };

    ARC_REFLECT_TYPE(LogFileSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "log.file", SettingScope::PreferencesProject, ApplyMode::Restart, Audience::Game)
        ARC_REFLECT_FIELD(LogFileSettings, keepCount)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 0.0, 100.0)
            ARC_REFLECT_ATTR(Tooltip, "Rotated engine log files kept beside the live one (name.1.log .. name.N.log). "
                                         "0 keeps none: the previous file is deleted.")
        ARC_REFLECT_FIELD(LogFileSettings, flushLevel)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev) ARC_REFLECT_ATTR(Range, 0.0, 6.0)
            ARC_REFLECT_ATTR(Apply, ApplyMode::Live)
            ARC_REFLECT_ATTR(Tooltip, "Log level at which the log file is flushed at once: 0 trace, 1 debug, 2 info, "
                                         "3 warn, 4 error, 5 critical, 6 never. Lower is more durable and slower.")
    ARC_END_REFLECT_TYPE()
}
