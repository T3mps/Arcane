#pragma once

// One process-wide TypeContext for the whole ArcaneTests run. Installed in the test
// module in main() (before Catch2 runs) and injected into every Runtime a test builds,
// so the test exe, Arcane.dll, and the loaded plugin all share one component-ID space.

#include <Astra/Core/TypeContext.hpp>

namespace Arcane::Test
{
    inline Astra::TypeContext& SharedTypeContext()
    {
        static Astra::TypeContext s_ctx;
        return s_ctx;
    }
}
