#include <Arcane/Render/PickEmit.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>

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
        // matrix's translation column (matrix[2]) is the world-space center;
        // the local-x column's angle is the world rotation; the column
        // magnitudes are the baked LocalTransform.scale (LocalTransform::ToMatrix
        // composes rotation*scale into columns 0/1), so half-extents = size*0.5
        // scaled per-axis by those magnitudes.
        {
            auto spriteView = registry.CreateView<WorldTransform, SpriteRenderer>();
            spriteView.ForEach([&](Astra::Entity e, WorldTransform& xf, SpriteRenderer& sp)
            {
                const glm::vec2 col0 = glm::vec2(xf.matrix[0]);
                const glm::vec2 col1 = glm::vec2(xf.matrix[1]);
                const glm::vec2 worldCenter = glm::vec2(xf.matrix[2]);
                const float sx = glm::length(col0);
                const float sy = glm::length(col1);
                const float angle = std::atan2(col0.y, col0.x);

                PickDrawable d;
                d.entity      = e;
                d.kind        = PickDrawable::Kind::Quad;
                d.center      = worldCenter * view.worldToScreenScale + view.offset;
                d.halfExtents = glm::vec2(sp.size.x * 0.5f * sx, sp.size.y * 0.5f * sy)
                                * view.worldToScreenScale;
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
            // unless the entity carries an authored LocalTransform.scale). Mirrors
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
}
