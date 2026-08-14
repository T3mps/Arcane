#include <Arcane/Render/PickEmit.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace Arcane
{
    namespace
    {
        // Rotate a body-local offset by the body's world angle (radians).
        glm::vec2 RotateVec(glm::vec2 v, float angle)
        {
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            return glm::vec2(v.x * c - v.y * s, v.x * s + v.y * c);
        }
    }

    void CollectPickables(Astra::Registry& registry, const PickView& view,
                          std::vector<PickDrawable>& out)
    {
        // ---- PASS 1: sprites -------------------------------------------------
        // Same OBB derivation as the retired CPU sprite-OBB pick: the world
        // matrix's translation column (matrix[2]) is the world-space PIVOT;
        // the local-x column's angle is the world rotation; the column
        // magnitudes are the baked Transform.scale (Transform::ToMatrix
        // composes rotation*scale into columns 0/1), so half-extents = the
        // sprite asset's base size * 0.5 scaled per-axis by those magnitudes.
        // Base size + pivot come from the SpriteTable exactly as submission
        // resolves them (RenderSystems.hpp) -- an unresolved or non-Rect sprite
        // is a 1x1 m quad at the center pivot -- so the silhouette keeps
        // matching the drawn quad.
        {
            const SpriteTable* spriteTable = registry.GetResource<SpriteTable>();
            auto spriteView = registry.CreateView<WorldTransform, SpriteRenderer>();
            spriteView.ForEach([&](Astra::Entity e, WorldTransform& xf, SpriteRenderer& sp)
            {
                const glm::vec2 col0 = glm::vec2(xf.matrix[0]);
                const glm::vec2 col1 = glm::vec2(xf.matrix[1]);
                const glm::vec2 worldPivot = glm::vec2(xf.matrix[2]);
                const float sx = glm::length(col0);
                const float sy = glm::length(col1);
                const float angle = std::atan2(col0.y, col0.x);

                const SpriteEntry* entry =
                    (sp.shape == SpriteShape::Rect && spriteTable)
                        ? spriteTable->Resolve(sp.sprite)
                        : nullptr;
                const glm::vec2 baseSize = entry ? entry->sizeMeters : glm::vec2(1.0f);
                const glm::vec2 pivot    = entry ? entry->pivot      : glm::vec2(0.5f);

                // World size of the drawn quad, and the pivot->center offset
                // turned by the body angle -- exactly zero at the default
                // center pivot, so this is the historical center for every
                // sprite that does not move its pivot.
                const glm::vec2 worldSize(baseSize.x * sx, baseSize.y * sy);
                const glm::vec2 worldCenter =
                    worldPivot + RotateVec((glm::vec2(0.5f) - pivot) * worldSize, angle);

                PickDrawable d;
                d.entity      = e;
                d.kind        = PickDrawable::Kind::Quad;
                d.center      = worldCenter * view.worldToScreenScale + view.offset;
                d.halfExtents = worldSize * 0.5f * view.worldToScreenScale;
                d.angle       = angle;
                out.push_back(d);
            });
        }

        // ---- PASS 2: physics colliders ---------------------------------------
        // One PickDrawable per Fixture on every live tracked body, iterated via
        // an archetype-stable View<Collider2D, PhysicsBodyRef> -- NOT the
        // PhysicsResource::entityToBody unordered_map. The drawable index IS the
        // hit-proxy id (id = index+1), so the order must be DETERMINISTIC: the
        // same rule PhysicsSystem's create pass follows ("order must not depend on
        // unordered_map hash/bucket layout"). The body pose is read from the live
        // PhysicsWorld via PhysicsBodyRef::handle so the silhouette registers with
        // the physics-debug overlay; fixture dims + local offset are scaled by
        // PhysicsBodyRef::appliedScale (the scale the create pass baked into the
        // body's fixtures, mirroring MakeScaledShape / MakeFixtureDef) so a scaled
        // body picks at its drawn size. Polygon fixtures carry no authored vertex
        // array (see PhysicsComponents.hpp) -- v1 approximates with the fixture's
        // halfW/halfH box, per design spec SS8.2 ("Polygon -> Box using its AABB").
        PhysicsResource* res = registry.GetResource<PhysicsResource>();
        if (!res || !res->world)
            return;

        Phys::PhysicsWorld& world = *res->world;

        auto colliderView = registry.CreateView<Collider2D, PhysicsBodyRef>();
        colliderView.ForEach([&](Astra::Entity entity, Collider2D& col, PhysicsBodyRef& ref)
        {
            if (ref.handle == Phys::kInvalidBody) return;
            if (!world.IsValid(ref.handle))       return;

            const Phys::Vec2 bp = world.Position(ref.handle);
            const glm::vec2  bodyPos(static_cast<float>(bp.x), static_cast<float>(bp.y));
            const float      bodyAngle = static_cast<float>(world.GetAngle(ref.handle));

            // Scale the create pass baked into this body's fixtures (identity
            // unless the entity carries an authored Transform.scale). Mirrors
            // PhysicsSystem::MakeScaledShape: per-axis for Aabb, |sx| length /
            // |sy| radius for Capsule, max(|sx|,|sy|) for Circle.
            const glm::vec2 scale = ref.appliedScale;
            const float     sx    = std::abs(scale.x);
            const float     sy    = std::abs(scale.y);
            const float     sMax  = std::max(sx, sy);

            for (const Fixture& fx : col.fixtures)
            {
                // Fixture local offset scales per-axis with the body's baked scale
                // (signed, matching MakeFixtureDef), then rotates into world space.
                const glm::vec2 localScaled(fx.localPos.x * scale.x, fx.localPos.y * scale.y);
                const glm::vec2 worldCenter  = bodyPos + RotateVec(localScaled, bodyAngle);
                const float     fixtureAngle = bodyAngle + fx.localAngle;
                const glm::vec2 canvasCenter = worldCenter * view.worldToScreenScale + view.offset;

                PickDrawable d;
                d.entity = entity;
                d.center = canvasCenter;
                d.angle  = fixtureAngle;

                switch (fx.kind)
                {
                case Phys::ShapeKind::Circle:
                    d.kind   = PickDrawable::Kind::Circle;
                    d.radius = fx.radius * sMax * view.worldToScreenScale;
                    break;
                case Phys::ShapeKind::Capsule:
                    d.kind    = PickDrawable::Kind::Capsule;
                    d.halfLen = fx.halfLen * sx * view.worldToScreenScale;
                    d.radius  = fx.radius  * sy * view.worldToScreenScale;
                    break;
                case Phys::ShapeKind::Aabb:
                    d.kind        = PickDrawable::Kind::Box;
                    d.halfExtents = glm::vec2(fx.halfW * sx, fx.halfH * sy) * view.worldToScreenScale;
                    break;
                case Phys::ShapeKind::Polygon:
                    // v1: no vertex data available -- fall back to the fixture's
                    // halfW/halfH box fields (scaled) as its AABB stand-in.
                    d.kind        = PickDrawable::Kind::Box;
                    d.halfExtents = glm::vec2(fx.halfW * sx, fx.halfH * sy) * view.worldToScreenScale;
                    break;
                }

                out.push_back(d);
            }
        });
    }

    uint32_t PickPassId(const std::vector<Astra::Entity>& ordered, Astra::Entity e)
    {
        if (e == Astra::Entity::Invalid())
            return 0u;
        for (std::size_t k = 0; k < ordered.size(); ++k)
            if (ordered[k] == e)
                return static_cast<uint32_t>(k + 1);
        return 0u;
    }

    glm::ivec2 PickSampleTexel(glm::vec2 pixel1x, uint32_t ss, uint32_t idW, uint32_t idH)
    {
        const int s = (int)ss;
        int x = (int)std::floor(pixel1x.x) * s + s / 2;
        int y = (int)std::floor(pixel1x.y) * s + s / 2;
        x = std::clamp(x, 0, (int)idW - 1);
        y = std::clamp(y, 0, (int)idH - 1);
        return glm::ivec2(x, y);
    }

    // ------------------------------------------------------------------
    // The id pass's geometry. Moved here VERBATIM from PickBuffer.cpp's
    // anonymous namespace (NRI Phase 2, Task 11) so the NVRHI recorder and the
    // graph's pick node share ONE emitter -- see the header's block comment.
    // ------------------------------------------------------------------

    uint32_t PickKindCode(PickDrawable::Kind kind)
    {
        switch (kind)
        {
        case PickDrawable::Kind::Quad:    return 0u;
        case PickDrawable::Kind::Circle:  return 1u;
        case PickDrawable::Kind::Capsule: return 2u;
        case PickDrawable::Kind::Box:     return 3u;
        }
        return 0u;
    }

    glm::vec2 PickBoundHalfExtents(const PickDrawable& d)
    {
        switch (d.kind)
        {
        case PickDrawable::Kind::Circle:  return glm::vec2(d.radius, d.radius);
        case PickDrawable::Kind::Capsule: return glm::vec2(d.halfLen + d.radius, d.radius);
        case PickDrawable::Kind::Quad:
        case PickDrawable::Kind::Box:
        default:                          return d.halfExtents;
        }
    }

    void BuildPickIdGeometry(std::span<const PickDrawable> drawables,
                             std::vector<PickIdVertex>& outVertices,
                             std::vector<uint32_t>& outIndices)
    {
        outVertices.clear();
        outIndices.clear();

        // Bounding-quad corner sign pattern: TL, TR, BR, BL.
        static const glm::vec2 kSigns[4] = {
            { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };

        for (std::size_t di = 0; di < drawables.size(); ++di)
        {
            const PickDrawable& d = drawables[di];
            const uint32_t id     = (uint32_t)di + 1u;   // 1-based
            const uint32_t code   = PickKindCode(d.kind);
            const glm::vec2 bound = PickBoundHalfExtents(d);

            // Rotate the bounding quad by the drawable's angle (the canvas map
            // has no rotation -- only scale + offset, already folded into the
            // drawable). The PS coverage test uses the UNROTATED `local`.
            const float c = std::cos(d.angle);
            const float s = std::sin(d.angle);
            const uint32_t base = (uint32_t)outVertices.size();

            for (int i = 0; i < 4; ++i)
            {
                const glm::vec2 local(kSigns[i].x * bound.x, kSigns[i].y * bound.y);
                const glm::vec2 rot(c * local.x - s * local.y,
                                    s * local.x + c * local.y);
                PickIdVertex v;
                v.pos     = d.center + rot;
                v.local   = local;
                v.radius  = d.radius;
                v.halfLen = d.halfLen;
                v.kind    = code;
                v.id      = id;
                outVertices.push_back(v);
            }

            const uint32_t quad[6] = { base, base + 1, base + 2,
                                       base, base + 2, base + 3 };
            outIndices.insert(outIndices.end(), quad, quad + 6);
        }
    }
}
