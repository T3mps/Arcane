#pragma once

#include <cstdint>
#include <string_view>

namespace arcbuild
{
    enum class HostPlatform : std::uint8_t
    {
        Windows,
        Linux,
        MacOS
    };

    [[nodiscard]]
    HostPlatform CurrentHostPlatform() noexcept;

    [[nodiscard]]
    std::string_view DefaultActionFor(
        HostPlatform platform) noexcept;

    enum class BuildBackend : std::uint8_t
    {
        None,
        MsBuild,
        Make,
        Ninja,
        XcodeBuild
    };

    [[nodiscard]]
    BuildBackend BackendForAction(std::string_view action) noexcept;

    [[nodiscard]]
    const char* BuildBackendName(
        BuildBackend backend);

    [[nodiscard]]
    const char* BuildBackendPrefix(
        BuildBackend backend);
}
