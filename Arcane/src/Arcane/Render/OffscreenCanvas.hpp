#pragma once

// Render module: a self-contained offscreen 2D pass. Owns a linear-HDR
// Canvas, a Batcher2D, a TonemapPass, and a display-referred (BGRA8_UNORM,
// gamma-encoded by the tonemap) output texture + framebuffer. Draw() runs
// the canonical canvas -> batcher ->
// tonemap path (the SAME path Loom drives) into the output texture and
// exposes it as an ImTextureID, so an ImGui panel can `ImGui::Image()` a
// real engine 2D render -- no parallel ImDrawList path grows beside it
// (homogenized-rendering mandate).
//
// Game-agnostic by design: the caller supplies the draw lambda; this helper
// knows nothing about what is drawn (physics overlays, Minkowski insets,
// thumbnails, ...).

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>

namespace Arcane
{
    class Batcher2D;
    class ShaderLibrary;

    class ARCANE_API OffscreenCanvas
    {
    public:
        // Returns null (with ARC_ERROR) if any owned resource fails to build.
        // `shaders` must outlive the returned OffscreenCanvas (the Batcher2D
        // and TonemapPass keep a reference for lazy pipeline rebuilds).
        static std::unique_ptr<OffscreenCanvas> Create(nvrhi::IDevice* device,
                                                       ShaderLibrary& shaders,
                                                       uint32_t width,
                                                       uint32_t height);

        virtual ~OffscreenCanvas() = default;

        // Clears the canvas to `clear` (linear), runs `fn` against the owned
        // Batcher2D (coordinates are canvas pixels, y down -- see Batcher2D),
        // tonemaps into the output texture, and submits the command list. The
        // output texture is shader-readable when this returns. NVRHI manages
        // all framebuffer transitions; no manual barriers.
        virtual void Draw(const std::function<void(Batcher2D&)>& fn,
                          glm::vec4 clear) = 0;

        // The output texture as an ImTextureID (ImU64), ready for
        // ImGui::Image(). The ImGui-NVRHI backend binds an arbitrary texture
        // by its raw pointer, so this is just (intptr_t)outputTexture. Stable
        // for the lifetime of the current targets; changes after Resize().
        virtual uint64_t TextureId() const = 0;

        // Tears down and rebuilds the canvas + output targets at the new size.
        // No-op on a zero dimension or an unchanged size.
        virtual void Resize(uint32_t width, uint32_t height) = 0;

        virtual uint32_t Width()  const = 0;
        virtual uint32_t Height() const = 0;
    };
}
