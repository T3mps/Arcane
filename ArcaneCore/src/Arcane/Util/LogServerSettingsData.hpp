#pragma once

// The log.server.* settings structs as plain data (settings arc S6-3): no
// reflection, std only. The header-only Logger (Util/Logger.hpp) is included
// by consumers that have no Astra include path (ArcaneAssetPipeline, and the
// services' code that never declares settings), so it reads the published
// values through the two ArcaneCore exports below. The reflection block and
// the registration live one header up, in LogServerSettings.hpp.
//
// The member initializers are the defaults: the pre-sweep literals of
// Logger.hpp (the console pattern was its console sink's, the file pattern its
// rotating sink's). SweepLogTest pins them.

#include <Arcane/Core/Api.hpp>

#include <cstdint>
#include <string>

namespace Arcane
{
    struct LogServerSettings
    {
        std::int32_t consoleLevel = 2;   // spdlog level: info
        std::int32_t fileLevel = 0;      // trace
        std::string  pattern = "%^[%H:%M:%S.%e] [%n] [%l]%$ %v";
        std::string  filePattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v";
        std::int32_t flushLevel = 2;     // info: structured JSON events reach the file at once
    };

    struct LogServerFileSettings
    {
        std::uint64_t maxBytes = 5ull << 20;   // 5 MiB per file
        std::int32_t  maxFiles = 3;            // rotated files kept
    };

    // Settings<T>() of the published snapshot, by value (the Logger keeps no
    // reference across a publish).
    ARC_CORE_API LogServerSettings PublishedLogServerSettings();
    ARC_CORE_API LogServerFileSettings PublishedLogServerFileSettings();
}
