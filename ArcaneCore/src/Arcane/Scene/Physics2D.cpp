// Arcane::Physics2D (input-seam spec 2026-10-02 s5.3): the game-facing physics
// commands as exported members of the published PhysicsResource. The body
// handle comes from entityToBody, never PhysicsBodyRef.

#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>

#include <cmath>

namespace Arcane
{
    namespace
    {
        bool HasFloorSupport(Phys::PhysicsWorld& world, Phys::BodyHandle handle)
        {
            bool supported = false;
            world.ForEachContactConstraint([&](const Phys::ContactConstraint& contact)
            {
                if (contact.bodyA == handle.index && contact.normal.y > Phys::Real(0.5))
                    supported = true;
                if (contact.bodyBIsBody && contact.bodyB == handle.index &&
                    contact.normal.y < Phys::Real(-0.5))
                    supported = true;
            });
            if (supported)
                return true;

            // A sleeping body's contacts need not appear in the active solver.
            // A resting body sits up to the linear slop INSIDE its support, and
            // a cast that starts overlapped answers t=0 with a zero normal
            // (Box2D-v3 parity), so the cast starts one slop higher and travels
            // one slop further: the reach below the feet stays 0.05 m.
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
                                                 Phys::Vec2(0, -(Phys::Real(0.05) + Phys::kLinearSlop)), opts,
                                                 world.GetFixtureWorldAngle(fixture));
                if (hit && hit->normal.y > Phys::Real(0.5))
                    return true;
            }
            return false;
        }
    }

    BodyMotion2D PhysicsResource::Motion(Astra::Entity entity, const RigidBody2D& body) const
    {
        BodyMotion2D motion;
        if (body.type != Phys::BodyType::Dynamic)
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
            motion.supported = HasFloorSupport(*world, it->second);
        return motion;
    }

    void PhysicsResource::SetVelocity(Astra::Entity entity, RigidBody2D& body,
                                      float velocityX, float velocityY)
    {
        if (!std::isfinite(velocityX) || !std::isfinite(velocityY))
            return;
        if (body.type != Phys::BodyType::Dynamic)
            return;
        body.velocity = glm::vec2(velocityX, velocityY);

        const auto it = entityToBody.find(entity);
        if (world && it != entityToBody.end() && world->IsValid(it->second))
            world->SetVelocity(it->second, Phys::Vec2(velocityX, velocityY));
    }
}
