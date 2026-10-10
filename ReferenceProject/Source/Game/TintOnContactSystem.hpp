#pragma once

// TintOnContactSystem: reads the frame's 2D physics events (pull model, spec
// 2026-10-08) and tints any TintOnContact entity named by a Arcane::Physics2D::ContactBegin.
// Update phase: FrameEvents() holds every fixed step of this frame.

#include <Arcane/Ecs.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsEvents2D.hpp>
#include <Arcane/Physics2D.hpp>

#include "TintOnContact.hpp"

namespace ReferenceProject
{
    struct TintOnContactSystem
    {
        void operator()(Arcane::ECS::View<TintOnContact, Arcane::SpriteRenderer>& view,
                        Arcane::ECS::Res<Arcane::Physics2D::World> physics)
        {
            const Arcane::Physics2D::Events events = physics->FrameEvents();
            if (events.contactBegin.empty()) return;
            view.ForEach([&](Arcane::ECS::Entity entity, TintOnContact& tint, Arcane::SpriteRenderer& sprite)
            {
                for (const Arcane::Physics2D::ContactBegin& e : events.contactBegin)
                {
                    if (e.a.entity == entity || e.b.entity == entity)
                    {
                        sprite.tint = tint.color;
                        return;
                    }
                }
            });
        }
    };
}
