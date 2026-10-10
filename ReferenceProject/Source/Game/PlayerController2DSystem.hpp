#pragma once

// PlayerController2DSystem: a system -- a functor the scheduler runs each fixed
// step. Its PARAMETERS say what it touches, so the scheduler can order and
// parallelise it: the controller + body view, the sim clock, gameplay input and
// the physics commands. The one trait is ordering: it moves the body before
// Arcane::Physics2D::System steps.
//
// PlayerController2DSystem.cpp declares the phase and role with ARC_SYSTEM;
// the game-module prologue discovers it. It reads locally resolved gameplay
// actions, so it runs on the client role; an authoritative network game would
// route commands to a server system.

#include <Arcane/Ecs.hpp>
#include <Arcane/Input/GameInput.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Physics2D.hpp>
#include <Arcane/Sim/Time.hpp>

#include "PlayerController2D.hpp"

#include <algorithm>

namespace ReferenceProject
{
    struct PlayerController2DSystem : Arcane::ECS::SystemTraits<Arcane::ECS::Before<Arcane::Physics2D::System>>
    {
        Arcane::ActionRef move{"Player", "Move"};
        Arcane::ActionRef jump{"Player", "Jump"};

        static float MoveTowards(float current, float target, float distance)
        {
            if (current < target)
            {
                return std::min(current + distance, target);
            }
            return std::max(current - distance, target);
        }

        void operator()(Arcane::ECS::View<PlayerController2D, Arcane::Physics2D::RigidBody>& view,
                        Arcane::ECS::Res<Arcane::Time> time,
                        Arcane::ECS::Res<Arcane::GameInput> input,
                        Arcane::ECS::ResMut<Arcane::Physics2D::World> physics)
        {
            const float dt       = std::clamp(static_cast<float>(time->fixedDt), 0.0f, 0.05f);
            const float axis     = input->Value(move).scalar;
            const bool  jumped   = input->PressedThisFixedStep(jump);
            const bool  jumpHeld = input->Down(jump);

            view.ForEach([&](Arcane::ECS::Entity entity, PlayerController2D& controller, Arcane::Physics2D::RigidBody& body)
            {
                const Arcane::Physics2D::BodyMotion motion = physics->Motion(entity, body);
                if (motion.supported)
                {
                    controller.coyoteRemaining = std::max(0.0f, controller.coyoteTime);
                    controller.jumpConsumed = false;
                    controller.jumpCutArmed = false;
                }
                if (jumped)
                    controller.jumpBufferRemaining = std::max(dt, controller.jumpBufferTime);

                const float inputX = std::clamp(axis, -1.0f, 1.0f);
                const float targetX = inputX * std::max(0.0f, controller.moveSpeed);
                // Braking and turns get their own rates so A/D responds quickly
                // without making midair direction changes feel identical to ground.
                float rate = motion.supported ? controller.groundAcceleration : controller.airAcceleration;
                if (motion.supported && inputX == 0.0f)
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
                if (controller.jumpCutArmed && !jumpHeld && nextY > 0.0f)
                {
                    nextY *= std::clamp(controller.jumpCutMultiplier, 0.0f, 1.0f);
                    controller.jumpCutArmed = false;
                }
                if (nextY <= 0.0f)
                {
                    controller.jumpCutArmed = false;
                }

                // Arcane::Physics2D::World handles both live and not-yet-minted bodies.
                physics->SetVelocity(entity, body, nextX, nextY);
                if (!motion.supported)
                {
                    controller.coyoteRemaining = std::max(0.0f, controller.coyoteRemaining - dt);
                }
                controller.jumpBufferRemaining = std::max(0.0f, controller.jumpBufferRemaining - dt);
            });
        }
    };
}
