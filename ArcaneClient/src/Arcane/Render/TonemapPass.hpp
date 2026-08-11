#pragma once

// Render module: the ONLY writer of display-referred output. Samples a
// linear HDR texture, applies Narkowicz ACES + the true IEC 61966-2-1
// piecewise sRGB encode (matching what SRGBA8_UNORM applies in hardware
// on input; the old gamma-2.2 client-oracle byte-match is retired), draws
// a fullscreen triangle into the target framebuffer.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <memory>

namespace Arcane
{
    class ShaderLibrary;

    class ARCANE_API TonemapPass
    {
    public:
        // Returns null (with ARC_ERROR) when the tonemap shaders are missing.
        static std::unique_ptr<TonemapPass> Create(nvrhi::IDevice* device,
                                                   ShaderLibrary& shaders);

        virtual ~TonemapPass() = default;

        // Records the fullscreen pass into an OPEN command list. The source
        // must be shader-readable; the target framebuffer is typically the
        // backbuffer. Pipelines rebuild lazily on shader Generation() bumps.
        virtual void Run(nvrhi::ICommandList* commandList,
                         nvrhi::ITexture*     source,
                         nvrhi::IFramebuffer* target) = 0;
    };
}
