#pragma once

// PickEmit: the CPU "which entities are pickable" seam for GPU entity-id
// picking (see docs/superpowers/specs/2026-07-19-arcane-entity-id-picking-design.md,
// SS3b/3c). CollectPickables walks the registry for pickable entities (sprites
// + physics colliders) and appends one PickDrawable per pickable shape, in
// canvas pixels, ready for the id-pass VS. The k-th appended drawable (0-based)
// gets hit-proxy id k+1; PickEntityForId inverts that mapping (id 0 == background).
//
// Pure, headless-testable: no GPU, no render device. This is the emitter half
// of the hit-proxy pass; PickBuffer (a later task) owns the R32_UINT target,
// the entity_id.hlsl pipeline, and the readback.
//
// PickView is the world->canvas mapping the Sandbox scene camera uses
// (Arcane/Sandbox/src/Camera.hpp): screen = world * worldToScreenScale + offset,
// a single combined scale plus a screen-space offset in canvas px, y-down, no
// y-flip. Width/height are NOT carried here -- PickBuffer owns those.

#include <Arcane/Base/Api.hpp>

#include <Astra/Entity/Entity.hpp>

#include <glm/vec2.hpp>

#include <cstdint>
#include <vector>

namespace Astra { class Registry; }

namespace Arcane
{
    // The world->canvas transform the scene render uses (matches Sandbox
    // Camera::WorldToScreen): canvas_px = worldPoint * worldToScreenScale + offset.
    struct PickView
    {
        glm::vec2 offset{0.0f, 0.0f};        // screen-space translation, canvas px
        float     worldToScreenScale = 1.0f; // px per world-meter (== Camera::WorldToScreenScale())
    };

    // A single pickable shape, already projected to CANVAS pixels (y-down),
    // ready for the id-pass vertex shader. `entity` is the owning entity;
    // `kind` selects which of the geometry fields are meaningful:
    //   Quad/Box -- center, halfExtents, angle
    //   Circle   -- center, radius
    //   Capsule  -- center, halfLen, radius, angle
    struct PickDrawable
    {
        Astra::Entity entity{};

        enum class Kind : uint8_t { Quad, Circle, Capsule, Box };
        Kind kind = Kind::Quad;

        glm::vec2 center{0.0f, 0.0f};
        glm::vec2 halfExtents{0.0f, 0.0f};
        float     radius = 0.0f;
        float     halfLen = 0.0f;
        float     angle = 0.0f;   // radians, world rotation (unaffected by the y-down canvas map)
    };

    // Collect every pickable entity's silhouette geometry, appended to `out`
    // (NOT cleared -- caller controls accumulation; pass an empty vector to
    // start fresh, matching how tests and PickBuffer both use it).
    //
    // ORDERING (this IS the hit-proxy id assignment: id = index+1):
    //   1. Sprites  -- entities with (WorldTransform, SpriteRenderer), in the
    //      registry's view iteration order (archetype-stable). One Quad per
    //      entity, derived from the world matrix + SpriteRenderer::size (same
    //      OBB math the retired CPU sprite-OBB pick used: center = translation
    //      column, angle = atan2 of the local-x column, half-extents = size*0.5
    //      scaled by the column magnitudes -- LocalTransform.scale baked into
    //      the world matrix), then projected to canvas via `view`.
    //   2. Colliders -- one PickDrawable per Fixture, iterated via an
    //      archetype-stable View<Collider2D, PhysicsBodyRef> (DETERMINISTIC:
    //      the id assignment id=index+1 must not depend on unordered_map hash
    //      order -- the same rule PhysicsSystem's create pass follows). The body
    //      pose comes from the live PhysicsWorld via PhysicsBodyRef::handle;
    //      fixture dims + local offset are scaled by PhysicsBodyRef::appliedScale
    //      so a scaled body's silhouette matches its drawn collider.
    // Documented choice: sprites are collected first (back), then colliders
    // (front) -- later drawables win a contested pixel in the id pass, so a
    // collider picks over an underlying sprite. Physics colliders are read via
    // registry.GetResource<PhysicsResource>(); if absent (no physics world on
    // this registry), only sprites are collected -- not an error.
    ARCANE_API void CollectPickables(Astra::Registry& registry, const PickView& view,
                                     std::vector<PickDrawable>& out);

    // id 0 -> background (invalid entity). id k (k>=1) -> drawables[k-1].entity.
    // Out-of-range k -> invalid entity. Astra::Entity{} is the invalid sentinel
    // (IsValid() == false).
    inline Astra::Entity PickEntityForId(const std::vector<PickDrawable>& drawables, uint32_t id)
    {
        if (id == 0 || id > drawables.size())
            return Astra::Entity{};
        return drawables[id - 1].entity;
    }
}
