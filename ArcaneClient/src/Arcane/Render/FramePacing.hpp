#pragma once

// Render module: how deep the CPU may run ahead of the GPU (settings arc S6-17).
//
// ONE NUMBER, REUSED RATHER THAN REINVENTED: the graph's command-buffer
// slots, upload-ring slots, descriptor-set arrays, pick readback regions and
// pacing-fence depth are ALL FramesInFlight(). It lives in a header of its own
// so a consumer that needs only the depth pulls in nothing else.
//
// TWO NAMES, TWO JOBS:
//   * kMaxFramesInFlight sizes every per-frame ARRAY (storage only);
//   * FramesInFlight() is the depth actually used for slots, pools, rings and
//     pacing: render.framesInFlight (2..3, Restart), clamped to the ceiling.
//
// LATCHED ONCE PER PROCESS: the first NriGraphContext calls
// LatchFramesInFlight() before it creates any per-frame resource, so every
// pool, ring and swapchain agrees for the life of the process. A read before
// that (a GpuScene made on a bare device, say) latches it instead; either way
// a later change to the cvar takes effect on the next launch.
//
// THE CONTRACT: in a host, nothing may call FramesInFlight() (nor create an
// NriGraphContext or GpuScene) before HostBoot::ApplyEarlyConfigRungs has
// published the config rungs. Such a read freezes whatever was published then
// (the default, before the rungs) for the whole process, silently overriding
// the user's render.framesInFlight. ApplyEarlyConfigRungs therefore calls
// CheckFramesInFlightLatch() right after its publish, which warns when an
// earlier read latched a depth other than the one just published. A process
// with no host (the tests) latches the default unless it sets the cvar first.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Core/Constant.hpp>

#include <cstdint>

namespace Arcane
{
    ARC_CONSTANT("storage ceiling for per-frame arrays; render.framesInFlight is clamped to it")
    inline constexpr std::uint32_t kMaxFramesInFlight = 3;

    // The CPU may run this many frames ahead of the GPU. Slot gating lives
    // INSIDE the swapchains; nothing above that interface sees it.
    [[nodiscard]] ARC_API std::uint32_t FramesInFlight() noexcept;

    // Fix FramesInFlight() from the published render.framesInFlight. The
    // first call wins; later calls are no-ops.
    ARC_API void LatchFramesInFlight() noexcept;

    // The early-latch diagnostic (see THE CONTRACT above). True when the depth
    // is not latched yet or is latched to the published render.framesInFlight;
    // false, with a warning, when an earlier read latched a different depth.
    // Never latches and never changes the depth.
    ARC_API bool CheckFramesInFlightLatch() noexcept;
}
