#pragma once

// RenderSubmissionSystem: reads WorldTransform + SpriteRenderer, submits one quad
// per sprite to the Batcher2D held in the RenderContext2D resource. Read-only
// w.r.t. ECS; the side effect (batcher submission) is external. Runs in the
// Render phase, single-threaded (Batcher2D is not thread-safe). The quad is
// rotated by the entity's WorldTransform rotation (passed to Batcher2D::Quad/
// Rect), so a sprite turns in lockstep with its rotating physics body.
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

#include <cmath>

namespace Arcane
{
    struct RenderSubmissionSystem
        : Astra::SystemTraits<Astra::Reads<WorldTransform, SpriteRenderer, PreviousTransform>>
    {
        void operator()(Astra::Registry& reg)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            if (!ctx || !ctx->batcher) return;
            const TextureTable* textures = reg.GetResource<TextureTable>();

            auto view = reg.CreateView<WorldTransform, SpriteRenderer>();
            view.ForEach([&](Astra::Entity e, WorldTransform& world, SpriteRenderer& sprite)
            {
                const glm::mat3& m = world.matrix;
                glm::vec2       worldPos(m[2].x, m[2].y);
                const glm::vec2 worldScale(glm::length(glm::vec2(m[0])),
                                           glm::length(glm::vec2(m[1])));
                // World rotation from the first basis column (matches
                // LocalTransform::ToMatrix: m[0] = (c*scale.x, s*scale.x)). The
                // camera applies a uniform zoom (no rotation), so the screen-space
                // sprite rotates by the same angle as its physics body.
                float worldRot = std::atan2(m[0].y, m[0].x);

                // Render interpolation (Epic 04.2): if the entity carries a
                // PreviousTransform (its prior fixed-step local pose, captured by
                // PhysicsSystem write-back), draw at lerp(prev -> current, alpha) for
                // smooth slow-mo. Rotation uses shortest-arc AngleLerp. Treats the
                // entity's local pose as its world pose -- exact for a flat / identity-
                // rooted physics entity (the case today). No PreviousTransform -> the
                // unchanged snap-to-step path.
                if (const PreviousTransform* prev = reg.GetComponent<PreviousTransform>(e))
                {
                    const float a = ctx->alpha;
                    worldPos = glm::vec2(Lerp(prev->position.x, worldPos.x, a),
                                         Lerp(prev->position.y, worldPos.y, a));
                    worldRot = AngleLerp(prev->rotation, worldRot, a);
                }
                // Apply the camera (screen = world * zoom + offset; matches
                // Sandbox::Camera::WorldToScreen and DrawPhysicsDebug exactly, so
                // sprites + the physics-debug overlay pan/zoom together): scale the
                // quad by zoom and center it on the ZOOM-scaled screen position.
                const glm::vec2 dstSize = sprite.size * worldScale * ctx->zoom;
                const glm::vec2 screenPos = worldPos * ctx->zoom + ctx->cameraOffset;
                // Center the sprite on the screen position (Batcher2D quads are
                // top-left-origin) so it aligns with the center-anchored physics
                // body + collider overlay.
                const glm::vec2 dstPos = screenPos - dstSize * 0.5f;

                ctx->batcher->SetLayer(static_cast<uint16_t>(sprite.sortingLayer),
                                       static_cast<uint16_t>(sprite.orderInLayer));

                // Draw the sprite's PRIMITIVE shape so it can match its collider.
                // Circle/Capsule go through the batcher's filled SDF primitives
                // (texture ignored); Rect keeps the textured/tinted rotated quad.
                switch (sprite.shape)
                {
                case SpriteShape::Circle:
                    // Filled disc, rotation-invariant. Diameter == dstSize.x.
                    ctx->batcher->Circle(screenPos, dstSize.x * 0.5f, sprite.tint);
                    break;
                case SpriteShape::Capsule:
                {
                    // Horizontal capsule (size.x >= size.y): a central band of
                    // length (size.x - size.y) and height size.y, plus two end
                    // discs of radius size.y/2 at the segment endpoints, all turned
                    // by worldRot about the center.
                    const float r = dstSize.y * 0.5f;
                    const glm::vec2 bandSize(dstSize.x - dstSize.y, dstSize.y);
                    ctx->batcher->Rect(screenPos - bandSize * 0.5f, bandSize,
                                       sprite.tint, worldRot);
                    const float halfLen = bandSize.x * 0.5f;
                    const float c = std::cos(worldRot), s = std::sin(worldRot);
                    ctx->batcher->Circle(glm::vec2(screenPos.x + c * halfLen,
                                                   screenPos.y + s * halfLen),
                                         r, sprite.tint);
                    ctx->batcher->Circle(glm::vec2(screenPos.x - c * halfLen,
                                                   screenPos.y - s * halfLen),
                                         r, sprite.tint);
                    break;
                }
                case SpriteShape::Rect:
                default:
                {
                    nvrhi::ITexture* tex = textures ? textures->Resolve(sprite.textureId) : nullptr;
                    if (tex)
                        ctx->batcher->Quad(dstPos, dstSize, tex,
                                           glm::vec2(0, 0), glm::vec2(1, 1),
                                           sprite.tint, worldRot);
                    else
                        ctx->batcher->Rect(dstPos, dstSize, sprite.tint, worldRot);
                    break;
                }
                }
            });
        }
    };
}
