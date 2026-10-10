#pragma once

// TintOnContactSystem: reads the frame's 2D physics events (pull model, spec
// 2026-10-08) and tints any TintOnContact entity named by a Arcane::ContactBegin2D.
// Update phase: FrameEvents() holds every fixed step of this frame.

#include <Arcane/Ecs.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Physics2D.hpp>

#include "TintOnContact.hpp"

namespace ReferenceProject
{
    struct TintOnContactSystem
    {
        void operator()(Arcane::View<TintOnContact, Arcane::SpriteRenderer>& view,
                        Arcane::Res<Arcane::PhysicsWorld2D> physics)
        {
            const Arcane::PhysicsEvents2D events = physics->FrameEvents();
            if (events.contactBegin.empty()) return;
            view.ForEach([&](Arcane::Entity entity, TintOnContact& tint, Arcane::SpriteRenderer& sprite)
            {
                for (const Arcane::ContactBegin2D& e : events.contactBegin)
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
