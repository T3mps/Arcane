#pragma once

#include <Arcane/Render/GraphicsBackend.hpp>
#include <cstdint>

namespace Arcane
{
    struct RenderGraphConfig
    {
        GraphicsBackend backend = kDefaultGraphicsBackend;
        bool vsync = true;
        std::uint64_t maxFrames = 0;
        bool pickProbe = false;
        std::int32_t pickProbeX = 0;
        std::int32_t pickProbeY = 0;
    };
}
