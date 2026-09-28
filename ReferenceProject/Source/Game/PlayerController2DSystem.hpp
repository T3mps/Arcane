#pragma once

// PlayerController2DSystem: a system -- a functor the scheduler runs over the registry each
// step. Declare what it reads and writes in the SystemTraits so the scheduler
// can order and parallelise it.
//
// PlayerController2DSystem.cpp declares the phase and role with ARCANE_SYSTEM;
// the game-module prologue discovers that factory without ReferenceGame.cpp
// knowing this type. Registrar discovery order is intentionally irrelevant:
// the Before<> trait below is the semantic ordering contract. Fixed update
// applies movement to the live body before PhysicsSystem steps and propagates
// its new pose. This sample reads a local keyboard, so it runs on the client
// role; an authoritative network game would route commands to a server system.

#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsCommands.hpp>
#include <Arcane/Scene/PhysicsSystem.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include "PlayerController2D.hpp"

#include <algorithm>

namespace ReferenceProject
{
    struct PlatformerControls
    {
        float horizontal = 0.0f;
        bool jumpPressed = false;
        bool jumpDown = false;
    };

    // Snapshot scancodes are SDL's physical key positions: A=4, D=7,
    // W=26, Space=44. Keep these bindings local to the sample game.
    struct PlatformerInputState
    {
        bool jumpHeld = false;

        PlatformerControls Sample(const Arcane::InputSnapshot& snapshot)
        {
            const bool live = !snapshot.wantCaptureKeyboard;
            const bool left = live && snapshot.ScancodeDown(4);
            const bool right = live && snapshot.ScancodeDown(7);
            const bool jump = live && (snapshot.ScancodeDown(26) || snapshot.ScancodeDown(44));
            const PlatformerControls controls
            {
                static_cast<float>(static_cast<int>(right) - static_cast<int>(left)),
                jump && !jumpHeld,
                jump
            };
            jumpHeld = jump;
            return controls;
        }
    };

    struct PlayerController2DSystem :
        Astra::SystemTraits<Astra::Writes<PlayerController2D>,
                            Astra::Writes<Arcane::RigidBody2D>,
                            Astra::Before<Arcane::PhysicsSystem>>
    {
        static float MoveTowards(float current, float target, float distance)
        {
            if (current < target)
            {
                return std::min(current + distance, target);
            }
            return std::max(current - distance, target);
        }

        void operator()(Astra::Registry& reg)
        {
            auto view = reg.CreateView<PlayerController2D, Arcane::RigidBody2D>();
            view.ForEach([&](Astra::Entity entity, PlayerController2D& controller, Arcane::RigidBody2D&)
            {
                const float dt = std::clamp(controller.fixedDt, 0.0f, 0.05f);
                const Arcane::BodyMotion2D motion = Arcane::GetBodyMotion2D(reg, entity);
                if (motion.supported)
                {
                    controller.coyoteRemaining = std::max(0.0f, controller.coyoteTime);
                    controller.jumpConsumed = false;
                    controller.jumpCutArmed = false;
                }
                if (controller.jumpRequested)
                    controller.jumpBufferRemaining = std::max(dt, controller.jumpBufferTime);

                const float input = std::clamp(controller.value, -1.0f, 1.0f);
                const float targetX = input * std::max(0.0f, controller.moveSpeed);
                // Braking and turns get their own rates so A/D responds quickly
                // without making midair direction changes feel identical to ground.
                float rate = motion.supported ? controller.groundAcceleration : controller.airAcceleration;
                if (motion.supported && input == 0.0f)
                {
                    rate = controller.groundBraking;
                }
                else if (motion.supported && motion.velocityX * targetX < 0.0f)
                {
                    rate = controller.turnAcceleration;
                }
                const float nextX = MoveTowards(motion.velocityX, targetX,
                                               std::max(0.0f, rate) * dt);
                float nextY = motion.velocityY;

                if (motion.bodyReady && controller.jumpBufferRemaining > 0.0f && controller.coyoteRemaining > 0.0f && !controller.jumpConsumed)
                {
                    nextY = std::max(0.0f, controller.jumpSpeed);
                    controller.jumpBufferRemaining = 0.0f;
                    controller.coyoteRemaining = 0.0f;
                    controller.jumpConsumed = true;
                    controller.jumpCutArmed = true;
                }
                // Releasing W/Space during ascent cuts the upward velocity once.
                // Gravity then finishes the short hop naturally; holding gives
                // the full arc without injecting extra force each frame.
                if (controller.jumpCutArmed && !controller.jumpHeld && nextY > 0.0f)
                {
                    nextY *= std::clamp(controller.jumpCutMultiplier, 0.0f, 1.0f);
                    controller.jumpCutArmed = false;
                }
                if (nextY <= 0.0f)
                {
                    controller.jumpCutArmed = false;
                }

                // The engine seam handles both live and not-yet-minted bodies.
                Arcane::SetBodyVelocity2D(reg, entity, nextX, nextY);
                if (!motion.supported)
                {
                    controller.coyoteRemaining = std::max(0.0f, controller.coyoteRemaining - dt);
                }
                controller.jumpBufferRemaining = std::max(0.0f, controller.jumpBufferRemaining - dt);
                controller.jumpRequested = false;
            });
        }
    };
}
