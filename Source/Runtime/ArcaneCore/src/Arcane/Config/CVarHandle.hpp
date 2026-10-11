#pragma once

#include <cstdint>

namespace Arcane
{
    // Index plus generation. A module unload bumps the generation, so a held
    // handle cannot read the slot's next occupant. Default-constructed is stale.
    struct CVarHandle
    {
        std::uint32_t index = 0xffffffffu;
        std::uint32_t generation = 0;

        constexpr bool IsStale() const noexcept { return index == 0xffffffffu; }
        friend constexpr bool operator==(CVarHandle a, CVarHandle b) noexcept
        {
            return a.index == b.index && a.generation == b.generation;
        }
    };
}
