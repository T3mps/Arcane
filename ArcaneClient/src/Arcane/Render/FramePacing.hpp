#pragma once

// Render module: how deep the CPU may run ahead of the GPU.
//
// ONE NUMBER, REUSED RATHER THAN REINVENTED: the graph's command-buffer
// slots, upload-ring slots, descriptor-set arrays, pick readback regions and
// pacing-fence depth are ALL this constant. It lives in a header of its own so
// a consumer that needs only the depth pulls in nothing else.

#include <cstdint>

namespace Arcane
{
    // CPU may run this many frames ahead of the GPU. Slot gating lives
    // INSIDE the swapchains; nothing above that interface sees it.
    inline constexpr uint32_t kSwapchainFramesInFlight = 2;
}
