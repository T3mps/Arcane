#pragma once

#include <cstdint>

namespace Arcane
{
    enum class MaterialBlendMode : std::uint8_t
    {
        Opaque = 0,
        Masked = 1,
        Transparent = 2,
    };
}
