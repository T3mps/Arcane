#include <Arcane/Scene/PhysicsCommands.hpp>

#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cmath>

// TEMPORARY forwarders (input-seam plan Task 9 -> deleted by Task 12): the
// ReferenceProject sources keep compiling until the controller moves to
// Arcane::Physics2D.
namespace Arcane
{
    BodyMotion2D GetBodyMotion2D(Astra::Registry& registry, Astra::Entity entity)
    {
        const RigidBody2D* body = registry.GetComponent<RigidBody2D>(entity);
        if (!body) return {};
        if (const PhysicsResource* physics = registry.GetResource<PhysicsResource>())
            return physics->Motion(entity, *body);
        BodyMotion2D motion;
        if (body->type == Phys::BodyType::Dynamic)
        {
            motion.velocityX = body->velocity.x;
            motion.velocityY = body->velocity.y;
        }
        return motion;
    }

    void SetBodyVelocity2D(Astra::Registry& registry, Astra::Entity entity, float velocityX, float velocityY)
    {
        RigidBody2D* body = registry.GetComponent<RigidBody2D>(entity);
        if (!body) return;
        if (PhysicsResource* physics = registry.GetResource<PhysicsResource>())
        {
            physics->SetVelocity(entity, *body, velocityX, velocityY);
            return;
        }
        if (std::isfinite(velocityX) && std::isfinite(velocityY) && body->type == Phys::BodyType::Dynamic)
            body->velocity = glm::vec2(velocityX, velocityY);
    }
}
