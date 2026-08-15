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

#include <cstddef>
#include <cstdint>
#include <span>
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
    //      entity, derived from the world matrix + the sprite ASSET's base size
    //      (SpriteTable, 1x1 m when unresolved) (same OBB math the retired CPU
    //      sprite-OBB pick used: angle = atan2 of the local-x column,
    //      half-extents = base size*0.5 scaled by the column magnitudes --
    //      Transform.scale baked into the world matrix -- and the centre offset
    //      from the translation column by the asset's pivot, zero at the
    //      default centre), then projected to canvas via `view`.
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

    // The pass id assigned to `e` under the k+1 convention CollectPickables emits
    // (the k-th entity in `ordered` gets id k+1; 0 = background). Reverse of the
    // read-back mapping in Pick(). 0 if `e` is absent or Astra::Entity::Invalid().
    ARCANE_API uint32_t PickPassId(const std::vector<Astra::Entity>& ordered, Astra::Entity e);

    // id 0 -> background (invalid entity). id k (k>=1) -> drawables[k-1].entity.
    // Out-of-range k -> invalid entity. Astra::Entity{} is the invalid sentinel
    // (IsValid() == false).
    inline Astra::Entity PickEntityForId(const std::vector<PickDrawable>& drawables, uint32_t id)
    {
        if (id == 0 || id > drawables.size())
            return Astra::Entity{};
        return drawables[id - 1].entity;
    }

    // The ENTITY-ONLY twin of PickEntityForId, same contract, for a consumer
    // that RETAINED the id<->entity table rather than the drawables it came
    // from. That is not a convenience: the NRI graph's pick readback lands
    // kSwapchainFramesInFlight frames after the id pass that produced it, so
    // the editor's deferred click-pick has to hold the table from the frame
    // that RASTERISED the click -- by which time the live drawables vector has
    // been rebuilt two or more times. Copying entities rather than whole
    // PickDrawables is what makes retaining it cheap.
    //
    // A DISTINCT NAME rather than an overload, deliberately: `PickPassId({}, e)`
    // and `PickEntityForId({}, id)` are both live call shapes in this tree, and
    // a braced empty argument against two container types is ambiguous rather
    // than convenient.
    inline Astra::Entity PickEntityForPassId(std::span<const Astra::Entity> ordered, uint32_t id)
    {
        if (id == 0 || id > ordered.size())
            return Astra::Entity{};
        return ordered[id - 1];
    }

    // PickPassId over the DRAWABLES rather than a separate entity vector -- the
    // form a caller that just ran CollectPickables already has in hand, and
    // therefore the form that cannot disagree with the id pass it just fed.
    // Same k+1 rule and the same FIRST-match tie-break (an entity with several
    // fixtures emits several drawables; the first is the one the outline
    // traces, which is what PickBuffer::PassIdOf reports too -- this is that
    // method's loop, against a span instead of a member).
    inline uint32_t PickPassIdOf(std::span<const PickDrawable> drawables, Astra::Entity e)
    {
        if (e == Astra::Entity::Invalid())
            return 0u;
        for (std::size_t k = 0; k < drawables.size(); ++k)
            if (drawables[k].entity == e)
                return static_cast<uint32_t>(k + 1);
        return 0u;
    }

    // The id-buffer texel to sample for a 1x viewport click at `pixel1x` when the
    // id buffer is supersampled by `ss` (center subsample), clamped to [0, dim).
    ARCANE_API glm::ivec2 PickSampleTexel(glm::vec2 pixel1x, uint32_t ss, uint32_t idW, uint32_t idH);

    // THE VIEWPORT ID PASS'S SUPERSAMPLE FACTOR -- ONE number for BOTH recorders
    // (NRI Phase 3, D3c). The id target is sized ss*width x ss*height while the
    // world->clip map stays LOGICAL, so the same silhouettes rasterise at ss x
    // density; outline_seed.hlsl then averages the ss*ss subsamples of each 1x
    // pixel into a SUB-PIXEL edge centroid (its `ctr`), which is the seed
    // position the composite measures its distance to.
    //
    // That makes this factor PIXEL-VISIBLE, not a quality knob: at ss=1 every
    // seed sits at its pixel centre, at ss=2 it sits up to a quarter-pixel off
    // it, and the composite's AA ramp is only 1 px wide -- so the two produce
    // visibly different outline edges. It used to be a literal 2 at
    // EditorApp's PickBuffer::Create and a literal 1 at PickNode::kSuperSample,
    // and the editor's `full` golden compare across the two arms is exactly
    // what found it. Both now read THIS, so they cannot drift apart again.
    inline constexpr uint32_t kPickSupersample = 2;

    // =====================================================================
    // THE ID PASS'S GEOMETRY -- one emitter, two recorders (NRI Phase 2,
    // Task 11).
    //
    // This used to be a file-local block inside PickBuffer.cpp. It moved here
    // when the NRI graph path grew its own pick node (Render/Nri/nodes/
    // PickOutlineNodes.cpp): both recorders MUST build the same vertices from
    // the same drawables in the same order, because the 1-based id a vertex
    // carries IS the id<->entity mapping every consumer inverts
    // (PickEntityForId). Two independent copies of this loop would be two id
    // assignments that agree until one of them is edited -- the same reasoning
    // that keeps ONE Batcher2D feeding both 2D recorders.
    //
    // Pure and headless: no render device, no nvrhi, no nri.
    // =====================================================================

    // Per-vertex data for the id pass, matching entity_id.hlsl's VSInput. The
    // C++ attribute array order at EITHER recorder MUST match that struct's
    // member order (nvrhi assigns Vulkan input locations by declaration order;
    // NRI takes an explicit vk.location, and D3D matches the custom semantic
    // name at SemanticIndex 0).
    struct PickIdVertex
    {
        glm::vec2 pos;      // canvas px: the rotated bounding-quad corner
        glm::vec2 local;    // shape-local coords (unrotated), canvas px
        float     radius;   // canvas px (circle/capsule)
        float     halfLen;  // canvas px (capsule)
        uint32_t  kind;     // 0=Quad 1=Circle 2=Capsule 3=Box
        uint32_t  id;       // 1-based hit-proxy id
    };
    static_assert(sizeof(PickIdVertex) == 32, "id vertex is the wire format");

    // kind -> the shader code entity_id.hlsl's PS switches on.
    ARCANE_API uint32_t PickKindCode(PickDrawable::Kind kind);

    // Bounding half-extents (canvas px) of a drawable's silhouette: the quad the
    // id pass rasterizes. The PS analytically discards fragments outside
    // circle/capsule shapes; Quad/Box fill the whole bound.
    ARCANE_API glm::vec2 PickBoundHalfExtents(const PickDrawable& drawable);

    // Build the id-pass vertex + index arrays from `drawables` (both vectors are
    // CLEARED first). One quad (4 verts / 6 indices) per drawable; the k-th
    // drawable (0-based) gets id k+1. Drawables are already ordered back-to-
    // front, so index order = draw order = front-most last (the output merger,
    // primitive-ordered, makes the last-drawn silhouette win a contested pixel
    // -- no depth buffer anywhere on this path).
    ARCANE_API void BuildPickIdGeometry(std::span<const PickDrawable> drawables,
                                        std::vector<PickIdVertex>& outVertices,
                                        std::vector<uint32_t>& outIndices);
}
