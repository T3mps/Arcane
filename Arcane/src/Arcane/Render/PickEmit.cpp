#include <Arcane/Render/PickEmit.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

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
        // Same OBB derivation as PickEntitiesAt (EntityPick.cpp): the world
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
        // One PickDrawable per Fixture on every live tracked body. Polygon
        // fixtures carry no authored vertex array on Fixture (see
        // PhysicsComponents.hpp) -- v1 approximates with the fixture's
        // halfW/halfH box fields, per design spec SS8.2 ("Polygon -> Box using
        // its AABB for v1").
        PhysicsResource* res = registry.GetResource<PhysicsResource>();
        if (!res || !res->world)
            return;

        Phys::PhysicsWorld& world = *res->world;
        for (const auto& [entity, handle] : res->entityToBody)
        {
            if (!world.IsValid(handle))
                continue;

            const Collider2D* col = registry.GetComponent<Collider2D>(entity);
            if (!col)
                continue;

            const Phys::Vec2 bp = world.Position(handle);
            const glm::vec2  bodyPos(static_cast<float>(bp.x), static_cast<float>(bp.y));
            const float      bodyAngle = static_cast<float>(world.GetAngle(handle));

            for (const Fixture& fx : col->fixtures)
            {
                const glm::vec2 worldCenter  = bodyPos + RotateVec(fx.localPos, bodyAngle);
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
                    d.radius = fx.radius * view.worldToScreenScale;
                    break;
                case Phys::ShapeKind::Capsule:
                    d.kind    = PickDrawable::Kind::Capsule;
                    d.halfLen = fx.halfLen * view.worldToScreenScale;
                    d.radius  = fx.radius * view.worldToScreenScale;
                    break;
                case Phys::ShapeKind::Aabb:
                    d.kind        = PickDrawable::Kind::Box;
                    d.halfExtents = glm::vec2(fx.halfW, fx.halfH) * view.worldToScreenScale;
                    break;
                case Phys::ShapeKind::Polygon:
                    // v1: no vertex data available -- fall back to the fixture's
                    // halfW/halfH box fields as its AABB stand-in.
                    d.kind        = PickDrawable::Kind::Box;
                    d.halfExtents = glm::vec2(fx.halfW, fx.halfH) * view.worldToScreenScale;
                    break;
                }

                out.push_back(d);
            }
        }
    }
}
