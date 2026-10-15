#pragma once

// The plain render.window.* struct (settings arc S6-24), kept free of the
// settings headers so Window.hpp can default WindowDesc from it: Settings.hpp
// pulls Astra's windows.h, whose wingdi ERROR macro breaks NRI's Message enum
// in every translation unit that includes NRI after Window.hpp.
//
// Its reflection (category, scope, apply mode, ranges, tooltips) lives in
// Arcane/Host/HostSettings.hpp, and HostSettings.cpp registers it. Every
// golden is captured at the 1280x720 default, so the default must not change.

#include <cstdint>

namespace Arcane
{
    struct RenderWindowSettings
    {
        std::uint32_t width     = 1280;
        std::uint32_t height    = 720;
        bool          resizable = true;
    };
}
