// The reporter's log-tail reader (crash window plan 2, task 6; spec §6
// "Window" content -- the log tail).
//
// Filesystem, NOT pure (it reads disk), but std-only: since node-page phase
// s8.1 it source-compiles into ArcaneTests AND ArcaneEditor (the crash
// viewer, CrashReportDocument), so it must stay free of windows.h --
// std::filesystem/std::ifstream cover the whole job.
#pragma once

#include <filesystem>
#include <string>

namespace Arcane::Reporter
{
    // The log a report shows: the folder's <stem>.log.txt first (self-contained
    // report, spec s5.6); `livePath` when the folder copy is missing; empty when
    // neither is a regular file. The ONE home of that lookup order (ReadLogTail
    // and the editor's crash viewer both resolve through it).
    [[nodiscard]] std::filesystem::path ResolveLogPath(const std::filesystem::path& stem, const std::filesystem::path& livePath);

    // The last `lines` lines of ResolveLogPath's file; "" when neither exists.
    [[nodiscard]] std::string ReadLogTail(const std::filesystem::path& stem, const std::filesystem::path& livePath, std::size_t lines);
}
