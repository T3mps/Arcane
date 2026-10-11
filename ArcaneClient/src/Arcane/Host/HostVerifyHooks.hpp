#pragma once

#include <Arcane/Client/Api.hpp>
#include <string>
#include <string_view>

namespace Arcane
{
    struct HostVerifyHooks
    {
        bool (*parseProbe)(std::string_view, std::string&) = nullptr;
        bool (*referenceNameIsSafe)(const std::string&) = nullptr;
    };

    ARC_CLIENT_API void SetHostVerifyHooks(HostVerifyHooks hooks) noexcept;
    ARC_CLIENT_API HostVerifyHooks GetHostVerifyHooks() noexcept;
}
