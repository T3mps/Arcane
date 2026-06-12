#pragma once

// Render module: backbuffer presentation against a Window. M1 pacing rule:
// Present() waits for GPU idle (one frame in flight) -- correct-first;
// real frame pacing arrives with the M2 renderer work. Present() promises
// presentation only: callers must NOT assume the GPU is idle afterwards.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace Arcane
{
    class ARCANE_API Swapchain
    {
    public:
        virtual ~Swapchain() = default;

        // Acquires the current backbuffer. Returns null when the frame must
        // be skipped (zero-sized window mid-resize, surface out of date);
        // callers skip rendering for that frame.
        virtual nvrhi::ITexture* BeginFrame() = 0;

        virtual void Present() = 0;

        virtual void Resize(uint32_t width, uint32_t height) = 0;

        virtual uint32_t Width() const = 0;
        virtual uint32_t Height() const = 0;
        virtual nvrhi::Format Format() const = 0;
    };
}
