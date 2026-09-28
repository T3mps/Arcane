#include <Arcane/Scene/PhysicsCommands.hpp>

#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>

#include <Astra/Registry/Registry.hpp>

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
            Phys::ShapeCastOpts opts;
            opts.movers = true;
            opts.exclude = handle;
            for (std::uint32_t i = 0; i < world.FixtureCount(handle); ++i)
            {
                const auto fixture = world.GetBodyFixture(handle, i);
                if (!world.IsValid(fixture))
                    continue;
                const auto hit = world.ShapeCast(world.GetFixtureShape(fixture),
                                                 world.GetFixtureWorldPos(fixture),
                                                 Phys::Vec2(0, Phys::Real(-0.05)), opts,
                                                 world.GetFixtureWorldAngle(fixture));
                if (hit && hit->normal.y > Phys::Real(0.5))
                    return true;
            }
            return false;
        }
    }

    BodyMotion2D GetBodyMotion2D(Astra::Registry& registry, Astra::Entity entity)
    {
        BodyMotion2D motion;
        const RigidBody2D* rigidBody = registry.GetComponent<RigidBody2D>(entity);
        if (!rigidBody || rigidBody->type != Phys::BodyType::Dynamic)
            return motion;
        motion.velocityX = rigidBody->velocity.x;
        motion.velocityY = rigidBody->velocity.y;

        PhysicsResource* physics = registry.GetResource<PhysicsResource>();
        const PhysicsBodyRef* body = registry.GetComponent<PhysicsBodyRef>(entity);
        if (!physics || !physics->world || !body || !physics->world->IsValid(body->handle))
            return motion;
        const Phys::Vec2 velocity = physics->world->Velocity(body->handle);
        motion.velocityX = static_cast<float>(velocity.x);
        motion.velocityY = static_cast<float>(velocity.y);
        motion.bodyReady = true;
        if (velocity.y <= Phys::Real(0))
            motion.supported = HasFloorSupport(*physics->world, body->handle);
        return motion;
    }

    void SetBodyVelocity2D(Astra::Registry& registry, Astra::Entity entity,
                           float velocityX, float velocityY)
    {
        if (!std::isfinite(velocityX) || !std::isfinite(velocityY))
            return;
        RigidBody2D* rigidBody = registry.GetComponent<RigidBody2D>(entity);
        if (!rigidBody || rigidBody->type != Phys::BodyType::Dynamic)
            return;
        rigidBody->velocity = glm::vec2(velocityX, velocityY);

        PhysicsResource* physics = registry.GetResource<PhysicsResource>();
        const PhysicsBodyRef* body = registry.GetComponent<PhysicsBodyRef>(entity);
        if (physics && physics->world && body && physics->world->IsValid(body->handle))
            physics->world->SetVelocity(body->handle, Phys::Vec2(velocityX, velocityY));
    }

    void SetBodyHorizontalVelocity(Astra::Registry& registry,
                                   Astra::Entity entity,
                                   float velocityX)
    {
        RigidBody2D* rigidBody = registry.GetComponent<RigidBody2D>(entity);
        if (!rigidBody)
            return;

        // PhysicsSystem reads this value on the first fixed step, when the
        // entity has no live PhysicsBodyRef yet.
        rigidBody->velocity.x = velocityX;

        PhysicsResource* physics = registry.GetResource<PhysicsResource>();
        if (!physics || !physics->world)
            return;
        const PhysicsBodyRef* body = registry.GetComponent<PhysicsBodyRef>(entity);
        if (!body || !physics->world->IsValid(body->handle))
            return;

        // The world owns velocity after minting. Use its actual vertical speed
        // so gravity and impulses survive this horizontal input command.
        const Phys::Vec2 current = physics->world->Velocity(body->handle);
        physics->world->SetVelocity(body->handle, Phys::Vec2(velocityX, current.y));
    }

    bool TryJumpBody(Astra::Registry& registry, Astra::Entity entity, float jumpSpeed)
    {
        if (!(jumpSpeed > 0.0f) || !std::isfinite(jumpSpeed))
            return false;
        const RigidBody2D* rigidBody = registry.GetComponent<RigidBody2D>(entity);
        if (!rigidBody || rigidBody->type != Phys::BodyType::Dynamic)
            return false;
        PhysicsResource* physics = registry.GetResource<PhysicsResource>();
        const PhysicsBodyRef* body = registry.GetComponent<PhysicsBodyRef>(entity);
        if (!physics || !physics->world || !body || !physics->world->IsValid(body->handle))
            return false;

        const Phys::Vec2 velocity = physics->world->Velocity(body->handle);
        if (velocity.y > Phys::Real(0))
            return false; // still leaving the previous jump/contact

        if (!HasFloorSupport(*physics->world, body->handle))
            return false;

        physics->world->SetVelocity(body->handle, Phys::Vec2(velocity.x, jumpSpeed));
        return true;
    }
}
