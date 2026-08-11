#pragma once

// Shared so the plugin module and the test module use the SAME C++ type for Pulse
// (one type name -> one TypeContext id). A per-TU anonymous-namespace struct would be
// a distinct type per module; do not rely on anonymous-namespace name-hash equality.

#include <Astra/Reflection/Reflection.hpp>

namespace Arcane::HotReloadTest
{
    struct Pulse { int ticks = 0; };

    ASTRA_REFLECT_TYPE(Pulse)
        ASTRA_REFLECT_FIELD(Pulse, ticks)
    ASTRA_END_REFLECT_TYPE()
}
