#pragma once

// Render module: backbuffer presentation against a Window.
// Pacing (M2): kSwapchainFramesInFlight frames in flight, slot-gated with
// nvrhi::EventQuery inside the swapchain implementations. Present() still
// promises presentation only -- callers must NOT assume any GPU sync
// happened. Anything needing the GPU drained calls waitForIdle() itself.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace Arcane
{
    // CPU may run this many frames ahead of the GPU. Slot gating lives
    // INSIDE the swapchains; nothing above this interface sees it.
    inline constexpr uint32_t kSwapchainFramesInFlight = 2;

    class ARCANE_API Swapchain
    {
    public:
        virtual ~Swapchain() = default;

        // Acquires the current backbuffer. Returns null when the frame must
        // be skipped (zero-sized window mid-resize, surface out of date);
        // callers skip rendering for that frame.
        virtual nvrhi::ITexture* BeginFrame() = 0;

        // Presents the acquired frame. Promises presentation, not synchronization.
        virtual void Present() = 0;

        // Width()/Height() afterwards reflect the ACTUAL surface extent --
        // on Vulkan/Win32 that is the window's real pixel size, which may
        // differ from the requested values. Callers should treat resize
        // events' pixel sizes as the source of truth.
        virtual void Resize(uint32_t width, uint32_t height) = 0;

        virtual uint32_t Width() const = 0;
        virtual uint32_t Height() const = 0;
        virtual nvrhi::Format Format() const = 0;
    };
}
