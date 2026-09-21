#include "Process.hpp"

#include <cstdio>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#endif

namespace arcbuild
{
#ifdef _WIN32
    namespace
    {
        std::wstring Widen(
            const std::string& utf8)
        {
            if (utf8.empty())
                return {};

            const int count =
                ::MultiByteToWideChar(
                    CP_UTF8,
                    0,
                    utf8.c_str(),
                    static_cast<int>(utf8.size()),
                    nullptr,
                    0);

            if (count <= 0)
                return {};

            std::wstring wide(
                static_cast<std::size_t>(count),
                L'\0');

            if (::MultiByteToWideChar(
                CP_UTF8,
                0,
                utf8.c_str(),
                static_cast<int>(utf8.size()),
                wide.data(),
                count) <= 0)
            {
                return {};
            }

            return wide;
        }
    }
#endif

    std::optional<int> ProcessRunner::RunStreaming(
        const std::string& commandLine,
        std::string_view prefix) const
    {
#ifdef _WIN32
        const std::wstring wide =
            Widen(commandLine);

        if (wide.empty())
            return std::nullopt;

        FILE* pipe =
            ::_wpopen(
                wide.c_str(),
                L"r");

        if (!pipe)
            return std::nullopt;

        char buffer[4096];

        while (std::fgets(
            buffer,
            sizeof(buffer),
            pipe))
        {
            std::string line(buffer);

            while (!line.empty() &&
                (line.back() == '\n' ||
                    line.back() == '\r'))
            {
                line.pop_back();
            }

            output_.Child(
                prefix,
                line);
        }

        const int status =
            ::_pclose(pipe);

        if (status == -1)
            return std::nullopt;

        return status;
#else
        (void)commandLine;
        (void)prefix;

        output_.Error(
            "process execution is not implemented on this platform yet");

        return std::nullopt;
#endif
    }
}