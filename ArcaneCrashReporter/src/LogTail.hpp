// The reporter's log-tail reader (crash window plan 2, task 6; spec §6
// "Window" content -- the log tail).
//
// Filesystem, NOT pure: unlike ReportView.hpp/.cpp this is NOT compiled into
// ArcaneTests (premake5.lua's ArcaneTests `files` list names ReportView.cpp
// only), so it is free to use whatever the standard library offers. It still
// stays free of windows.h -- std::filesystem/std::ifstream cover the whole
// job -- but nothing requires that here; it just happens to need nothing
// more.
#pragma once

#include <filesystem>
#include <string>

namespace Arcane::Reporter
{
    // The folder's <stem>.log.txt first (self-contained report, spec s5.6);
    // the live log file's tail when the folder copy is missing; "" when neither.
    [[nodiscard]] std::string ReadLogTail(const std::filesystem::path& stem, const std::filesystem::path& livePath, std::size_t lines);
}
