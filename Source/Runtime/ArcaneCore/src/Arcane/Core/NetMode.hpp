#pragma once

#include <Arcane/Core/Api.hpp>
#include <cstdint>

namespace Arcane
{
    enum class NetMode : std::uint8_t { Standalone, DedicatedServer, ListenServer, Client };
    [[nodiscard]] ARC_CORE_API const char* ToString(NetMode mode) noexcept;
}
