#pragma once

// RenderSubmissionSystem: reads WorldTransform + SpriteRenderer, submits one quad
// per sprite to the Batcher2D held in the RenderContext2D resource. Read-only
// w.r.t. ECS; the side effect (batcher submission) is external. Runs in the
// Render phase, single-threaded (Batcher2D is not thread-safe). The quad is
// rotated by the entity's WorldTransform rotation (passed to Batcher2D::Quad/
// Rect), so a sprite turns in lockstep with its rotating physics body.
//
// Sprite anchor = the sprite asset's PIVOT: the entity's world position is the
// point the quad is placed and rotated about. The default pivot (0.5, 0.5) puts
// that at the quad's center, which is the historical behavior and lines the
// sprite up with its physics body / collider (also center-anchored --
// PhysicsDebugDraw draws the collider outline centered on the body position).
// The Batcher2D Quad/Rect primitives take a TOP-LEFT origin, hence the shift
// from center to dstPos here.
//
// Size comes from the sprite ASSET (SpriteEntry::sizeMeters, resolved through
// the SpriteTable resource) times the entity's world scale -- SpriteRenderer
// carries no size of its own. Primitives (Circle/Capsule) and unresolved
// sprites use a 1x1 m base, so their scale IS their size in meters.

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <glm/glm.hpp>

#include <cmath>

namespace Arcane
{
    // Reads<> is honest now: the view below is const (Astra adoption 2026-09-11).
    struct RenderSubmissionSystem
        : Astra::SystemTraits<Astra::Reads<WorldTransform, SpriteRenderer, PreviousTransform, Hidden>>
    {
        void operator()(Astra::Registry& reg)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            if (!ctx || !ctx->batcher) return;
            const SpriteTable* spriteTable = reg.GetResource<SpriteTable>();
            const SpriteMaterialTable* materials = reg.GetResource<SpriteMaterialTable>();
            const PhysicsInterpBuffer* interp = reg.GetResource<PhysicsInterpBuffer>();

            auto view = reg.CreateView<const WorldTransform, const SpriteRenderer, Astra::Not<Hidden>>();
            view.ForEach([&](Astra::Entity e, const WorldTransform& world, const SpriteRenderer& sprite)
            {
                // Task 3 (F1): the world matrix is a mat4 now, so the
                // translation is COLUMN 3 (it was column 2). Everything below
                // still reads the XY PLANE ONLY and ignores Z -- a sprite is a
                // screen-space quad, not a world quad, and making it one is F5's
                // question, not this task's. The basis lengths are likewise
                // taken from the 2D projection of columns 0/1, which is what the
                // mat3 path measured, so a planar entity gets a byte-identical
                // size.
                const glm::mat4& m = world.matrix;
                glm::vec2       worldPos(m[3].x, m[3].y);
                const glm::vec2 worldScale(glm::length(glm::vec2(m[0])),
                                           glm::length(glm::vec2(m[1])));
                // World rotation from the first basis column (matches
                // Transform::ToMatrix: for a Z-axis turn m[0] = (c*scale.x,
                // s*scale.x, 0)). The camera applies a uniform zoom (no
                // rotation), so the screen-space sprite rotates by the same
                // angle as its physics body.
                float worldRot = std::atan2(m[0].y, m[0].x);

                // Render interpolation (Epic 04.2, re-based 2026-09-11 spec s8): a
                // physics body's PREVIOUS world-slot pose lives in PhysicsInterpBuffer
                // (captured by PhysicsSystem PASS 2.5 before each step, indexed by
                // PhysicsWorld body SLOT; slotOf is this entity's address into it,
                // rebuilt by the same capture). Blend position (XY) and angle from it
                // to the current WORLD pose by alpha -- Lerp/AngleLerp, the debug
                // overlay's own helpers, so sprite and overlay agree to the bit.
                // World-slot poses are world poses, MORE correct than the retired
                // PreviousTransform path's local-as-world approximation. ANY miss --
                // no buffer, not yet captured, no entry for this entity, slot past
                // the buffer, or a recycled slot (generation mismatch) -- is the
                // unchanged snap-to-step.
                if (interp && interp->captured)
                {
                    if (const InterpSlot* slot = interp->slotOf.TryGet(e))
                    {
                        if (slot->index < interp->prev.size()
                            && interp->prev[slot->index].generation == slot->generation)
                        {
                            const InterpPose& pp = interp->prev[slot->index];
                            worldPos = glm::vec2(Lerp(pp.position.x, worldPos.x, ctx->alpha),
                                                 Lerp(pp.position.y, worldPos.y, ctx->alpha));
                            worldRot = AngleLerp(pp.angle, worldRot, ctx->alpha);
                        }
                    }
                }
                // Only a Rect consults the sprite asset: Circle/Capsule exist to
                // MATCH a collider, so they must stay on the 1x1 m base times
                // scale -- an asset's size/pivot would drift them off their body.
                const SpriteEntry* entry =
                    (sprite.shape == SpriteShape::Rect && spriteTable)
                        ? spriteTable->Resolve(sprite.sprite)
                        : nullptr;
                // Primitives and unresolved sprites draw a 1x1 m base; the sprite
                // asset supplies the base for textured rects. Scale (not a
                // component field) is the sizing mechanism -- see the 2026-07-28
                // sprite-asset spec.
                const glm::vec2 baseSize = entry ? entry->sizeMeters : glm::vec2(1.0f);
                const glm::vec2 pivot    = entry ? entry->pivot      : glm::vec2(0.5f);
                // Apply the camera (screen = world * zoom + offset; matches
                // Sandbox::Camera::WorldToScreen and DrawPhysicsDebug exactly, so
                // sprites + the physics-debug overlay pan/zoom together): scale the
                // quad by zoom and place it against the ZOOM-scaled screen position.
                const glm::vec2 dstSize = baseSize * worldScale * ctx->zoom;
                const glm::vec2 screenPos = worldPos * ctx->zoom + ctx->cameraOffset;
                // The batcher rotates a quad about its CENTER (QuadCorners,
                // Batcher2D.hpp:56-57: center = pos + half, corners rotated about
                // center). The entity position is the PIVOT, so place the center at
                // pivot + R(worldRot) * (pivot->center offset) -- which reduces to
                // dstPos = screenPos - dstSize * 0.5f at the default center pivot
                // (centerOff is then exactly zero), the historical placement.
                const glm::vec2 centerOff = (glm::vec2(0.5f) - pivot) * dstSize;
                const float cr = std::cos(worldRot), sr = std::sin(worldRot);
                const glm::vec2 center(screenPos.x + cr * centerOff.x - sr * centerOff.y,
                                       screenPos.y + sr * centerOff.x + cr * centerOff.y);
                const glm::vec2 dstPos = center - dstSize * 0.5f;

                ctx->batcher->SetLayer(static_cast<uint16_t>(sprite.sortingLayer),
                                       static_cast<uint16_t>(sprite.orderInLayer));

                // Draw the sprite's PRIMITIVE shape so it can match its collider.
                // Circle/Capsule go through the batcher's filled SDF primitives
                // (no sprite asset at all -- `entry` is null by construction
                // above, so they are centered on screenPos); Rect keeps the
                // textured/tinted rotated quad.
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
                    // Texture identity + UVs come from the resolved sprite
                    // asset (its pixel sub-rect, normalized by
                    // ComputeSpriteGeom). No asset, or an asset whose source
                    // texture is nil: a nil Guid and the full-range UVs a
                    // full-texture sprite would have anyway.
                    const Guid texId = entry ? entry->textureId : Guid::Nil();
                    const glm::vec2 uvMin = entry ? entry->uvMin : glm::vec2(0.0f, 0.0f);
                    const glm::vec2 uvMax = entry ? entry->uvMax : glm::vec2(1.0f, 1.0f);
                    // Sprite material (Slice 8): a valid Guid resolves to a
                    // registered Batcher2D material id; 0 (unresolved / nil) is
                    // the plain sprite path -- byte-identical when no sprite in
                    // the scene carries a material.
                    const uint16_t materialId =
                        materials && sprite.material.IsValid()
                            ? materials->Resolve(sprite.material) : 0;
                    // QuadTextured IS QuadMaterial plus the identity: same
                    // material, same vertices, one Guid more. The three-way
                    // branch is by (has material, has image): a registered
                    // material always goes through the material arm, a bare
                    // image through the built-in sprite arm, and a sprite that
                    // names neither falls back to a tint Rect so it still
                    // draws.
                    if (materialId != 0)
                        ctx->batcher->QuadTextured(materialId, texId, dstPos, dstSize,
                                                   uvMin, uvMax,
                                                   sprite.tint, worldRot);
                    else if (texId.IsValid())
                        ctx->batcher->QuadTextured(Batcher2D::kMaterialSprite, texId,
                                                   dstPos, dstSize,
                                                   uvMin, uvMax,
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
