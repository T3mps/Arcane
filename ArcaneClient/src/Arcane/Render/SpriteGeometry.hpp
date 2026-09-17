#pragma once

// SpriteWorldQuad: THE sprite corner rule (F4 plan 1 T5). One place derives
// the four WORLD-space corners of a sprite from its world matrix, its base
// size in metres and its pivot, so every consumer -- RenderSubmissionSystem
// (the drawn quad), the editor's framing bounds, and in plan 2 the pick
// emitter -- places the sprite identically.
//
// Local corners live in the sprite's own XY plane, +Y up = the IMAGE TOP:
//
//     TL = (-pivot.x * w, (1 - pivot.y) * h)    TR = ((1 - pivot.x) * w, (1 - pivot.y) * h)
//     BL = (-pivot.x * w,     -pivot.y  * h)    BR = ((1 - pivot.x) * w,     -pivot.y  * h)
//
// The pivot is normalized over the image: (0,0) = BOTTOM-left, (1,1) =
// top-right, (0.5,0.5) = centre (SpriteAsset.hpp, spec 2026-09-17 s2). The
// local origin IS the pivot, so the entity's world position is the point the
// sprite is placed and turned about. Each corner then goes through the FULL
// basis, `world * vec4(local, 0, 1)`: scale rides the matrix (so `baseSize`
// is the asset's metres, not pre-scaled), a Z turn spins the quad in the
// plane, and an X or Y tilt lays it into 3D -- a sprite is a world quad now,
// not a screen-space one.
//
// Header-only, glm-only: no Batcher2D, no NRI, no ECS. Order TL, TR, BR, BL
// matches Batcher2D::QuadWorld (TL keeps uvMin, so the image top lands on the
// +Y edge).

#include <glm/glm.hpp>

#include <array>

namespace Arcane
{
    struct SpriteQuad
    {
        std::array<glm::vec3, 4> corners;   // TL, TR, BR, BL in the sprite's plane, +Y up = image top
    };

    [[nodiscard]] inline SpriteQuad SpriteWorldQuad(const glm::mat4& world,
                                                    glm::vec2        baseSizeMetres,
                                                    glm::vec2        pivot) noexcept
    {
        const float w = baseSizeMetres.x;
        const float h = baseSizeMetres.y;
        const float left   = -pivot.x * w;
        const float right  = (1.0f - pivot.x) * w;
        const float bottom = -pivot.y * h;
        const float top    = (1.0f - pivot.y) * h;
        const glm::vec2 local[4] = {
            { left,  top    },   // TL
            { right, top    },   // TR
            { right, bottom },   // BR
            { left,  bottom },   // BL
        };
        SpriteQuad q;
        for (int i = 0; i < 4; ++i)
            q.corners[static_cast<std::size_t>(i)] =
                glm::vec3(world * glm::vec4(local[i].x, local[i].y, 0.0f, 1.0f));
        return q;
    }
}
