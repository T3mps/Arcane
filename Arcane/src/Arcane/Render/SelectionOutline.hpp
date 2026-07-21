#pragma once

// A per-frame GPU screen-space edge-detect that draws two-color EXTERIOR outlines
// (selected + hovered) from an R32_UINT hit-proxy id buffer, composited over a
// display-referred target. Editor- and game-agnostic. Sibling of TonemapPass.
// Design: docs/superpowers/specs/2026-07-21-arcane-selection-outline-design.md.

#include <Arcane/Base/Api.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <memory>

namespace Arcane
{
    class ShaderLibrary;

    class ARCANE_API SelectionOutline
    {
    public:
        static std::unique_ptr<SelectionOutline> Create(nvrhi::IDevice*, ShaderLibrary&);
        virtual ~SelectionOutline() = default;

        struct Params
        {
            uint32_t   selectedId = 0;                              // 0 = no selection
            glm::ivec2 cursorPx   = { -1, -1 };                     // viewport-local; <0 => no hover
            glm::vec4  selectColor = { 1.0f, 0.65f, 0.10f, 1.0f };  // amber (display-referred)
            glm::vec4  hoverColor  = { 0.25f, 0.70f, 1.0f, 1.0f };  // cyan  (display-referred)
            uint32_t   selectThicknessPx = 3;
            uint32_t   hoverThicknessPx  = 2;
        };

        // Edge-detect `idBuffer` (R32_UINT) and composite the two-color outlines into
        // `target` on the OPEN command list. hoveredId is sampled in-shader from
        // Params::cursorPx (no readback). Select takes precedence over hover.
        virtual void Render(nvrhi::ICommandList*,
                            nvrhi::ITexture* idBuffer,
                            nvrhi::IFramebuffer* target,
                            const Params&) = 0;
    };
}
