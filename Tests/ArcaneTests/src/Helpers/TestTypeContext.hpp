#pragma once

// One process-wide TypeContext for the whole ArcaneTests run. Installed in the test
// module in main() (before Catch2 runs) and injected into every Runtime a test builds,
// so the test exe, Arcane.dll, and the loaded plugin all share one component-ID space.

#include <Astra/Core/TypeContext.hpp>
#include <Arcane/Base/ProcessContext.hpp>

#include <cstdlib>
#include <memory>

namespace Arcane::Test
{
    inline Astra::TypeContext& SharedTypeContext()
    {
        static Astra::TypeContext s_ctx;
        return s_ctx;
    }

    // The test process's ONE ProcessContext, ADOPTING SharedTypeContext() (so the exe's
    // own per-module install at test_main stays the same object). Created here, on first
    // use, which test_main forces before Catch2 runs.
    inline Arcane::ProcessContext& Process()
    {
        static std::unique_ptr<Arcane::ProcessContext> s_pc = [] {
            Arcane::ProcessContextDesc d; d.externalTypeContext = &SharedTypeContext();
            auto pc = Arcane::ProcessContext::Create(d);
            if (!pc) std::abort();   // a second Create in the test exe is a harness bug, not a test
            return pc;
        }();
        return *s_pc;
    }
}
