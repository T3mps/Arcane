#pragma once

// PlayerController2DSystem: a system -- a functor the scheduler runs over the registry each
// step. Declare what it reads and writes in the SystemTraits so the scheduler
// can order and parallelise it.
//
// PlayerController2DSystem.cpp declares the phase and role with ARCANE_SYSTEM;
// the game-module prologue discovers that factory without ReferenceGame.cpp
// knowing this type. Registrar discovery order is intentionally irrelevant:
// the Before<> trait below is the semantic ordering contract. Fixed update runs
// this movement before transform propagation so the same step sees the new
// local transform. Both roles suit this reference input/movement probe; a real
// project should choose Server for authoritative-only simulation or Client for
// presentation-only behavior.

#include <Arcane/Scene/TransformSystems.hpp>   // the placement anchor
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include "PlayerController2D.hpp"

namespace ReferenceProject
{
    struct PlayerController2DSystem :
        Astra::SystemTraits<Astra::Reads<PlayerController2D>,
                            Astra::Writes<Arcane::RigidBody2D>,
                            Astra::Before<Arcane::TransformPropagationSystem>>
    {
        void operator()(Astra::Registry& reg)
        {
            reg.CreateView<const PlayerController2D, Arcane::RigidBody2D>().ForEach([](Astra::Entity entity, const PlayerController2D& controller, Arcane::RigidBody2D& rigidBody)
            {
                rigidBody.velocity.x = controller.value * controller.moveSpeed;
            });
        }
    };
}
