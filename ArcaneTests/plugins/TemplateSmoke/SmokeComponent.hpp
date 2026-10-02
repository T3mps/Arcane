#pragma once

// SmokeComponent: a component -- plain data on an entity. Reflected so the editor's
// Inspector can show and edit it, scenes can save it, and the Add Component
// catalog can offer it. Registered with the game module by the
// ARCANE_COMPONENT line in SmokeComponent.cpp; nothing else to wire.

#include <Arcane/Reflection.hpp>

namespace TemplateSmoke
{
    struct SmokeComponent
    {
        float value = 0.0f;
    };

    ARCANE_REFLECT_TYPE(SmokeComponent)
        ARCANE_REFLECT_FIELD(SmokeComponent, value)
    ARCANE_END_REFLECT_TYPE()
}
