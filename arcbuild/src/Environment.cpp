#include "Environment.hpp"

#include <cstdlib>

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
    std::optional<std::filesystem::path>
        Environment::ArcaneSdk() const
    {
#ifdef _WIN32
        const wchar_t* value =
            ::_wgetenv(L"ARCANE_SDK");

        if (!value || !*value)
            return std::nullopt;

        return std::filesystem::path(value);
#else
        const char* value =
            std::getenv("ARCANE_SDK");

        if (!value || !*value)
            return std::nullopt;

        return std::filesystem::path(value);
#endif
    }

    bool Environment::SetArcaneSdk(
        const std::filesystem::path& root) const
    {
#ifdef _WIN32
        return ::SetEnvironmentVariableW(
            L"ARCANE_SDK",
            root.wstring().c_str()) != FALSE;
#else
        return ::setenv(
            "ARCANE_SDK",
            root.string().c_str(),
            1) == 0;
#endif
    }
}