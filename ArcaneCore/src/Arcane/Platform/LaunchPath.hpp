#pragma once

// The check every launch site makes before it runs a program it was handed
// (settings S7-SEC). CVarFlags::LaunchesProgram keeps project config from
// naming a program at all; this is the defence in depth behind it, at the
// call sites that run one: Toolchain's build.* overrides (arcbuild's
// ProcessRunner, IdeLaunch), the crash reporter (Diagnostics) and arcbuild's
// own ProcessRunner.
//
// A launchable path is non-empty, holds no '"', CR or LF (the characters that
// end a quoted token or a cmd.exe line), and names something that exists and
// is not a directory: a regular file, or on Windows a reparse point such as
// an app-execution alias (WindowsApps\*.exe), which std::filesystem does not
// report as a regular file. Windows reads the attributes with
// GetFileAttributesW, so a relative path resolves against the working
// directory, as CreateProcessW's lpApplicationName does.

#include <Arcane/Core/Api.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Arcane
{
    enum class LaunchPathStatus : std::uint8_t
    {
        Ok,
        Empty,
        UnsafeCharacters,   // '"', CR or LF
        NotFound,
        NotAFile,           // a directory or a device
    };

    [[nodiscard]] ARC_CORE_API LaunchPathStatus CheckLaunchPath(const std::filesystem::path& program);

    // "is empty", "contains a quote or a line break", "does not exist", "is not a file".
    [[nodiscard]] ARC_CORE_API std::string_view LaunchPathStatusText(LaunchPathStatus status) noexcept;

    // Whether `text` holds a character that can end a quoted command-line
    // token or a cmd.exe line: '"', CR or LF.
    [[nodiscard]] ARC_CORE_API bool HasCommandLineBreaker(std::string_view text) noexcept;

    // One argv token -> its spelling inside a Win32 command line, by
    // CommandLineToArgvW's rules: returned untouched when it needs no quoting
    // (non-empty, no space, tab or quote); otherwise wrapped in quotes with
    // every backslash run that precedes a literal quote, or the closing
    // wrapper, doubled. The one implementation the CreateProcessW callers
    // share (RuntimeLaunch::QuoteArg forwards here).
    [[nodiscard]] ARC_CORE_API std::wstring QuoteWindowsArg(const std::wstring& arg);
}
