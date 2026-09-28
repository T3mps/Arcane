#pragma once

// A small game-module-safe command seam for controller-driven 2D motion.
// PhysicsWorld is linked inside ArcaneCore, so an external module must call
// an exported function rather than link Manifold2D's methods itself.

#include <Arcane/Core/Api.hpp>

#include <Astra/Entity/Entity.hpp>

namespace Astra { class Registry; }

namespace Arcane
{
    struct BodyMotion2D
    {
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        bool bodyReady = false;
        bool supported = false;
    };

    // Read the live dynamic body's velocity and floor support. Before minting,
    // velocity comes from RigidBody2D and bodyReady is false.
    ARCANE_CORE_API BodyMotion2D GetBodyMotion2D(Astra::Registry& registry, Astra::Entity entity);

    // Set both axes on an existing body, or its authored mint velocity.
    ARCANE_CORE_API void SetBodyVelocity2D(Astra::Registry& registry, Astra::Entity entity, float velocityX, float velocityY);

    // Set authored X velocity for a body that has not been minted yet, or set
    // the live body's X velocity while preserving its current vertical speed.
    // Call from fixed update before PhysicsSystem. Missing bodies are harmless.
    ARCANE_CORE_API void SetBodyHorizontalVelocity(Astra::Registry& registry, Astra::Entity entity, float velocityX);

    // Launch a supported dynamic body upward, preserving X speed. Returns
    // false for air, walls, ceilings, missing/stale bodies or invalid speed.
    ARCANE_CORE_API bool TryJumpBody(Astra::Registry& registry, Astra::Entity entity, float jumpSpeed);
}
