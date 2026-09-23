// UTF-16 <-> UTF-8 for the reporter's Win32 files (crash window plan 2).
// Inline and header-only: the entry point takes its argv from
// CommandLineToArgvW (wide), the whole rest of the program speaks UTF-8
// because that is what the envelope is, and every Win32 call in between needs
// the wide form back. Deliberately NOT in ReporterArgs.hpp -- that TU is pure
// and source-compiles into ArcaneTests, which must not pull windows.h.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <string_view>

namespace Arcane::Reporter
{
    inline std::wstring ToWide(std::string_view s)
    {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n > 0 ? static_cast<std::size_t>(n) : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    inline std::string ToUtf8(std::wstring_view w)
    {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }
}
