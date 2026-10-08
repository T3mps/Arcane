#pragma once

// log.server.* (settings arc S6-3, inventory Part 1 "Base / Diagnostics /
// Log"): the header-only server Logger (Util/Logger.hpp) that the services
// consume. Logger::Init reads them; its first call registers the callbacks
// that re-apply the Live rows to a running Logger at each publish. The structs
// are plain data in LogServerSettingsData.hpp (the Logger's consumers need no
// Astra include path); this header adds the reflection blocks.

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Util/LogServerSettingsData.hpp>

namespace Arcane
{
    ARC_REFLECT_TYPE(LogServerSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "log.server", SettingScope::Project, ApplyMode::Live, Audience::Server)
        ARC_REFLECT_FIELD(LogServerSettings, consoleLevel)
            ARC_REFLECT_ATTR(Range, 0.0, 6.0)
            ARC_REFLECT_ATTR(Tooltip, "Service console log level: 0 trace, 1 debug, 2 info, 3 warn, 4 error, "
                                         "5 critical, 6 off. A level the service passes to Logger::Init holds until this changes.")
        ARC_REFLECT_FIELD(LogServerSettings, fileLevel)
            ARC_REFLECT_ATTR(Range, 0.0, 6.0)
            ARC_REFLECT_ATTR(Tooltip, "Service log FILE level: 0 trace .. 6 off. A level the service passes to "
                                         "Logger::Init holds until this changes.")
        ARC_REFLECT_FIELD(LogServerSettings, pattern)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "spdlog pattern of the service console log lines.")
        ARC_REFLECT_FIELD(LogServerSettings, filePattern)
            ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "spdlog pattern of the service log file lines.")
        ARC_REFLECT_FIELD(LogServerSettings, flushLevel)
            ARC_REFLECT_ATTR(Range, 0.0, 6.0)
            ARC_REFLECT_ATTR(Tooltip, "Log level at which the service logs are flushed at once: 0 trace .. 2 info .. "
                                         "6 never. Lower is more durable and slower.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(LogServerFileSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "log.server.file", SettingScope::Project, ApplyMode::Restart, Audience::Server)
        ARC_REFLECT_FIELD(LogServerFileSettings, maxBytes)
            ARC_REFLECT_ATTR(Range, 65536.0, 1073741824.0)
            ARC_REFLECT_ATTR(Tooltip, "Size (bytes) at which the service log file rotates.")
        ARC_REFLECT_FIELD(LogServerFileSettings, maxFiles)
            ARC_REFLECT_ATTR(Range, 1.0, 100.0)
            ARC_REFLECT_ATTR(Tooltip, "Rotated service log files kept.")
    ARC_END_REFLECT_TYPE()
}
