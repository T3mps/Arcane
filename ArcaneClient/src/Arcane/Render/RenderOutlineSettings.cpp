#include <Arcane/Render/RenderOutlineSettings.hpp>

#include <algorithm>

ARC_SETTINGS(Arcane::RenderOutlineSettings);

namespace Arcane
{
    namespace
    {
        // The registered ranges, re-applied: a hand-edited file is refused at
        // Set, but the latch never trusts a value it sizes GPU work with.
        std::uint32_t ReadMaxThicknessPx() noexcept { return std::clamp(Settings<RenderOutlineSettings>().maxThicknessPx, 1u, 256u); }
        std::uint32_t ReadSupersample() noexcept { return std::clamp(Settings<RenderOutlineSettings>().supersample, 1u, 4u); }
    }

    // Function-local statics: the first call (NriGraphContext's node creation,
    // after the early config rungs) fixes the value for the process -- the
    // Restart contract. A later Set reaches the next launch.
    std::uint32_t OutlineMaxThicknessPx() noexcept
    {
        static const std::uint32_t latched = ReadMaxThicknessPx();
        return latched;
    }

    std::uint32_t PickSupersample() noexcept
    {
        static const std::uint32_t latched = ReadSupersample();
        return latched;
    }
}
