#pragma once

// RenderSubmissionSystem: reads WorldTransform + SpriteRenderer, submits one quad
// per sprite to the Batcher2D held in the RenderContext2D resource. Read-only
// w.r.t. ECS; the side effect (batcher submission) is external. Runs in the
// Render phase, single-threaded (Batcher2D is not thread-safe). Quads are axis
// aligned for the slice (rotation is ignored by Batcher2D::Quad).
//
// Sprite anchor = CENTER: a sprite is drawn centered on its entity's world
// position (dstPos = worldPos - dstSize/2), so it lines up with that entity's
// physics body / collider, which are also center-anchored (PhysicsDebugDraw
// draws the collider outline centered on the body position). The Batcher2D
// Quad/Rect primitives take a TOP-LEFT origin, hence the half-size shift here.

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <glm/glm.hpp>

namespace Arcane
{
    struct RenderSubmissionSystem
        : Astra::SystemTraits<Astra::Reads<WorldTransform, SpriteRenderer>>
    {
        void operator()(Astra::Registry& reg)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            if (!ctx || !ctx->batcher) return;
            const TextureTable* textures = reg.GetResource<TextureTable>();

            auto view = reg.CreateView<WorldTransform, SpriteRenderer>();
            view.ForEach([&](Astra::Entity, WorldTransform& world, SpriteRenderer& sprite)
            {
                const glm::mat3& m = world.matrix;
                const glm::vec2 worldPos(m[2].x, m[2].y);
                const glm::vec2 worldScale(glm::length(glm::vec2(m[0])),
                                           glm::length(glm::vec2(m[1])));
                const glm::vec2 dstSize = sprite.size * worldScale;
                // Center the sprite on the entity world position (Batcher2D quads
                // are top-left-origin) so it aligns with the center-anchored
                // physics body + collider overlay.
                const glm::vec2 dstPos =
                    worldPos + ctx->cameraOffset - dstSize * 0.5f;

                ctx->batcher->SetLayer(static_cast<uint16_t>(sprite.sortingLayer),
                                       static_cast<uint16_t>(sprite.orderInLayer));

                nvrhi::ITexture* tex = textures ? textures->Resolve(sprite.textureId) : nullptr;
                if (tex)
                    ctx->batcher->Quad(dstPos, dstSize, tex,
                                       glm::vec2(0, 0), glm::vec2(1, 1), sprite.tint);
                else
                    ctx->batcher->Rect(dstPos, dstSize, sprite.tint);
            });
        }
    };
}
