#pragma once

#include <Arcane/Core/Constant.hpp>
#include <Arcane/Core/Api.hpp>
#include <cstdint>

namespace Arcane
{
    ARC_CONSTANT("ABI: the game-module ABI version; the host refuses a mismatched module")
    inline constexpr std::uint32_t kGamePluginABIVersion = 58;
    ARC_CORE_API std::uint32_t PluginABIVersion();
}
