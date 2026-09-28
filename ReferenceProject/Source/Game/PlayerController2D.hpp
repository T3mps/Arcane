#pragma once

// PlayerController2D: a component -- plain data on an entity. Reflected so the editor's
// Inspector can show and edit it, scenes can save it, and the Add Component
// catalog can offer it. Registered with the game module by the
// ARCANE_COMPONENT line in PlayerController2D.cpp; nothing else to wire.

#include <Astra/Reflection/Reflection.hpp>
namespace ReferenceProject
{
    struct PlayerController2D
    {
        float value = 0.0f;

        float moveSpeed = 0.0f;
    };

    ASTRA_REFLECT_TYPE(PlayerController2D)
        ASTRA_REFLECT_FIELD(PlayerController2D, value)
        ASTRA_REFLECT_FIELD(PlayerController2D, moveSpeed)
    ASTRA_END_REFLECT_TYPE()
}
