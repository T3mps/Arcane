#pragma once

// TintOnContact: a component -- when a contact begins on this entity, its
// SpriteRenderer takes `color`. The 2D physics events witness (spec 2026-10-08
// s9.3): it proves a game module reads PhysicsWorld2D::FrameEvents().

#include <Arcane/Reflection.hpp>
#include <glm/vec4.hpp>

namespace ReferenceProject
{
    struct TintOnContact
    {
        glm::vec4 color{ 1.0f, 0.0f, 0.0f, 1.0f };
    };

    ARC_REFLECT_TYPE(TintOnContact)
        ARC_REFLECT_FIELD(TintOnContact, color)
    ARC_END_REFLECT_TYPE()
}
