// Read/write a whole text file, for the reporter's own small artifacts
// (crash window plan 2).
//
// R44: ONE home. The plan wrote these two bodies twice -- once inline in
// ReporterMain.cpp (task 5's sibling write) and once in the file task 9
// creates for the monitor -- which is verbatim-duplicated logic a reviewer
// would (correctly) flag. They live here instead and both callers include it.
//
// PURE and std-only, like ReporterArgs.hpp and SymbolizedText.hpp: no
// windows.h, so nothing stops a later task from source-compiling a consumer
// into ArcaneTests. The std::filesystem::path overloads of ifstream/ofstream
// open the NATIVE (wide) form as-is, which is the whole reason the callers
// hand a path rather than a narrowed string (R64).
#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace Arcane::Reporter
{
    // The file's bytes, or "" when it cannot be opened. Binary: a report's
    // text is written with explicit '\n' and must round-trip unchanged.
    [[nodiscard]] inline std::string Slurp(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        if (!in) return {};
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    // false when the stream could not be opened OR the write itself failed --
    // which is a state the caller must ACT on (R64): the sibling is the only
    // artifact of the whole hand-off, and a silent success after a failed
    // write is the lie ExitCode::kWriteFailed exists to remove.
    inline bool WriteText(const std::filesystem::path& p, std::string_view text)
    {
        std::ofstream out(p, std::ios::binary);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        return static_cast<bool>(out);
    }
}
