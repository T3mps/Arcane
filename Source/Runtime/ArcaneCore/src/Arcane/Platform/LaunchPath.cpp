#include <Arcane/Platform/LaunchPath.hpp>

#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Arcane
{
    LaunchPathStatus CheckLaunchPath(const std::filesystem::path& program)
    {
        const std::filesystem::path::string_type& native = program.native();
        if (native.empty())
            return LaunchPathStatus::Empty;
        for (const auto c : native)
            if (c == '"' || c == '\r' || c == '\n')
                return LaunchPathStatus::UnsafeCharacters;
#ifdef _WIN32
        const DWORD attributes = ::GetFileAttributesW(native.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return LaunchPathStatus::NotFound;
        if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) != 0)
            return LaunchPathStatus::NotAFile;
        // A regular file, or a reparse point to one (an app-execution alias
        // or a file symlink): CreateProcessW runs either.
        return LaunchPathStatus::Ok;
#else
        std::error_code ec;
        const std::filesystem::file_status status = std::filesystem::status(program, ec);
        if (ec || !std::filesystem::exists(status))
            return LaunchPathStatus::NotFound;
        return std::filesystem::is_regular_file(status) ? LaunchPathStatus::Ok : LaunchPathStatus::NotAFile;
#endif
    }

    std::string_view LaunchPathStatusText(LaunchPathStatus status) noexcept
    {
        switch (status)
        {
        case LaunchPathStatus::Ok:               return "is launchable";
        case LaunchPathStatus::Empty:            return "is empty";
        case LaunchPathStatus::UnsafeCharacters: return "contains a quote or a line break";
        case LaunchPathStatus::NotFound:         return "does not exist";
        case LaunchPathStatus::NotAFile:         return "is not a file";
        }
        return "is not launchable";
    }

    bool HasCommandLineBreaker(std::string_view text) noexcept
    {
        return text.find_first_of("\"\r\n") != std::string_view::npos;
    }

    std::wstring QuoteWindowsArg(const std::wstring& arg)
    {
        if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
            return arg;

        std::wstring out = L"\"";
        std::size_t backslashes = 0;
        for (wchar_t c : arg)
        {
            if (c == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (c == L'"')
            {
                out.append(backslashes * 2 + 1, L'\\');
                backslashes = 0;
                out.push_back(L'"');
                continue;
            }
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(c);
        }
        out.append(backslashes * 2, L'\\');
        out.push_back(L'"');
        return out;
    }
}
