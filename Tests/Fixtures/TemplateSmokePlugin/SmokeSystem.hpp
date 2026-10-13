#pragma once

// SmokeSystem: a system -- a functor the scheduler runs each step. Its
// PARAMETERS say what it touches, so the scheduler can order and parallelise
// it: views over components and engine resources such as the sim clock.
//
//     void operator()(Arcane::View<Arcane::Transform>& view,
//                     Arcane::Res<Arcane::Time> time,
//                     Arcane::Res<Arcane::GameInput> input)   // #include <Arcane/Input/GameInput.hpp>
//
// Fixed-update systems run before transform propagation by default so gameplay
// can move local transforms first. Registrar discovery order is irrelevant:
// scheduler order is expressed only through Before<> and After<> traits.
// The ARC_SYSTEM declaration that selects phase and network role is in
// SmokeSystem.cpp.

#include <Arcane/Ecs.hpp>
#include <Arcane/Scene/TransformSystems.hpp>   // the placement anchor

namespace TemplateSmoke
{
    struct SmokeSystem : Arcane::SystemTraits<Arcane::Before<Arcane::TransformPropagationSystem>>
    {
        void operator()(Arcane::Res<Arcane::Time> time)
        {
            (void)time;
        }
    };
}
