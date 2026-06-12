#pragma once

// Render module: the linear-HDR scene target. ALL scene rendering happens
// here in linear space (RGBA16_FLOAT); only TonemapPass writes the
// display-referred backbuffer. Resize recreates texture + framebuffer.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>

namespace Arcane
{
    class ARCANE_API Canvas
    {
    public:
        virtual ~Canvas() = default;

        virtual nvrhi::ITexture*     Texture()     const = 0;
        virtual nvrhi::IFramebuffer* Framebuffer() const = 0;
        virtual void     Resize(uint32_t width, uint32_t height) = 0;
        virtual uint32_t Width()  const = 0;
        virtual uint32_t Height() const = 0;
    };

    // Returns null (with ARC_ERROR) on creation failure.
    ARCANE_API std::unique_ptr<Canvas> CreateCanvas(nvrhi::IDevice* device,
                                                    uint32_t width,
                                                    uint32_t height);
}
