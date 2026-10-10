#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/PhysicsQuerySettings.hpp>

#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include <cmath>
#include <utility>

namespace
{
    namespace Phys = Arcane::Physics2D::Detail::Phys;
}

namespace Arcane::Physics2D
{
    World::~World() = default;
    World::World(World&&) noexcept = default;
    World& World::operator=(World&&) noexcept = default;

    namespace
    {
        bool HasFloorSupport(Phys::PhysicsWorld& world, Phys::BodyHandle handle, const GroundSettings& ground)
        {
            const Phys::Real minY = Phys::Real(ground.minNormalY);
            bool supported = false;
            world.ForEachContactConstraint([&](const Phys::ContactConstraint& contact)
            {
                if (contact.bodyA == handle.index && contact.normal.y > minY)
                    supported = true;
                if (contact.bodyBIsBody && contact.bodyB == handle.index &&
                    contact.normal.y < -minY)
                    supported = true;
            });
            if (supported)
                return true;

            Phys::ShapeCastOpts opts;
            opts.movers = true;
            opts.exclude = handle;
            for (std::uint32_t i = 0; i < world.FixtureCount(handle); ++i)
            {
                const auto fixture = world.GetBodyFixture(handle, i);
                if (!world.IsValid(fixture))
                    continue;
                const Phys::Vec2 origin = world.GetFixtureWorldPos(fixture);
                const auto hit = world.ShapeCast(world.GetFixtureShape(fixture),
                                                 Phys::Vec2(origin.x, origin.y + Phys::kLinearSlop),
                                                 Phys::Vec2(0, -(Phys::Real(ground.probeDistance) + Phys::kLinearSlop)), opts,
                                                 world.GetFixtureWorldAngle(fixture));
                if (hit && hit->normal.y > minY)
                    return true;
            }
            return false;
        }
    }

    BodyMotion World::Motion(Arcane::ECS::Entity entity, const RigidBody& body) const
    {
        BodyMotion motion;
        if (body.type != BodyType::Dynamic)
            return motion;
        motion.velocityX = body.velocity.x;
        motion.velocityY = body.velocity.y;

        const auto it = entityToBody.find(entity);
        if (!world || it == entityToBody.end() || !world->IsValid(it->second))
            return motion;
        const Phys::Vec2 velocity = world->Velocity(it->second);
        motion.velocityX = static_cast<float>(velocity.x);
        motion.velocityY = static_cast<float>(velocity.y);
        motion.bodyReady = true;
        if (velocity.y <= Phys::Real(0))
            motion.supported = HasFloorSupport(*world, it->second, Settings<GroundSettings>());
        return motion;
    }

    void World::SetVelocity(Arcane::ECS::Entity entity, RigidBody& body,
                            float velocityX, float velocityY)
    {
        if (!std::isfinite(velocityX) || !std::isfinite(velocityY))
            return;
        if (body.type != BodyType::Dynamic)
            return;
        body.velocity = glm::vec2(velocityX, velocityY);

        const auto it = entityToBody.find(entity);
        if (world && it != entityToBody.end() && world->IsValid(it->second))
            world->SetVelocity(it->second, Phys::Vec2(velocityX, velocityY));
    }
}
