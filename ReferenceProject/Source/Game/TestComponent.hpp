#pragma once

// TestComponent: a component -- plain data on an entity. Reflected so the editor's
// Inspector can show and edit it, scenes can save it, and the Add Component
// catalog can offer it. Registered with the game module by the
// ARCANE_COMPONENT line in TestComponent.cpp; nothing else to wire.

#include <Astra/Reflection/Reflection.hpp>

namespace ReferenceProject
{
    struct TestComponent
    {
        float value = 0.0f;
    };

    ASTRA_REFLECT_TYPE(TestComponent)
        ASTRA_REFLECT_FIELD(TestComponent, value)
    ASTRA_END_REFLECT_TYPE()
}
