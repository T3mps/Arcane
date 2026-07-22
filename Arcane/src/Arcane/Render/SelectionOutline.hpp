#pragma once

// A per-frame GPU Jump-Flood distance-field outline: seed (from a SUPERSAMPLED
// R32_UINT id buffer) -> JFA -> composite two-color outlines that STRADDLE the
// silhouette edge (selected + hovered) into a display-referred target. Owns its
// render targets. Editor- and
// game-agnostic. Design: docs/superpowers/specs/2026-07-21-arcane-selection-outline-jfa-design.md.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <memory>

namespace Arcane
{
    class ShaderLibrary;

    // Number of jump-flood passes to resolve distances up to `maxThicknessPx`:
    // ceil(log2(maxThicknessPx)) + 1 (jumps 2^(N-1) .. 1).
    ARCANE_API uint32_t JfaPassCount(uint32_t maxThicknessPx);

    class ARCANE_API SelectionOutline
    {
    public:
        static std::unique_ptr<SelectionOutline> Create(nvrhi::IDevice*, ShaderLibrary&,
                                                        uint32_t width, uint32_t height);
        virtual ~SelectionOutline() = default;

        struct Params
        {
            uint32_t   selectedId = 0;                              // 0 = no selection
            glm::ivec2 cursorPx   = { -1, -1 };                     // viewport-local; <0 => no hover
            glm::vec4  selectColor = { 1.0f, 0.65f, 0.10f, 1.0f };  // amber (display-referred)
            glm::vec4  hoverColor  = { 0.25f, 0.70f, 1.0f, 1.0f };  // cyan  (display-referred)
            float      selectThicknessPx = 3.0f;                    // full width (px), centered on the edge
            float      hoverThicknessPx  = 3.0f;                    // full width (px), centered on the edge
            float      edgeSoftnessPx    = 1.0f;                    // AA ramp width
        };

        // Seed -> JFA -> composite. `idBuffer` is the SUPERSAMPLED R32_UINT id target;
        // the supersample factor is derived from idBuffer size / this pass's size.
        // hoveredId is sampled in-shader from Params::cursorPx (no readback). Owns its
        // ping-pong targets at Create/Resize size. Select takes precedence over hover.
        virtual void Render(nvrhi::ICommandList*,
                            nvrhi::ITexture* idBuffer,
                            nvrhi::IFramebuffer* target,
                            const Params&) = 0;

        virtual void Resize(uint32_t width, uint32_t height) = 0;

        // The final JFA distance-field target after the last Render (nearest-seed
        // RGBA16). For debug visualization + tests; not needed by consumers.
        virtual nvrhi::ITexture* DebugDistanceField() const = 0;
    };
}
