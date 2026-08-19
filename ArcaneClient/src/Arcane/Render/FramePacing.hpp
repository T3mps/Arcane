#pragma once

// Render module: how deep the CPU may run ahead of the GPU.
//
// NRI Phase 5a, Task 8a: this constant used to live in Render/Swapchain.hpp,
// the NVRHI presentation interface this phase deletes. Most of that header's
// includers wanted only this line: the graph's command-buffer slots,
// upload-ring slots, descriptor-set arrays, pick readback regions and pacing
// fence depth are ALL this one number, and NriSwapChain.hpp's own header
// comment calls it out as "reused, not reinvented". So it moved to a home
// with no nvrhi dependency, ahead of the deletion rather than during it.
//
// The value and its meaning are unchanged.

#include <cstdint>

namespace Arcane
{
    // CPU may run this many frames ahead of the GPU. Slot gating lives
    // INSIDE the swapchains; nothing above that interface sees it.
    inline constexpr uint32_t kSwapchainFramesInFlight = 2;
}
