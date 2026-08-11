// ArcaneTests runner entry point (Server CommonTests convention).
// Add new test files under Arcane/ArcaneTests/src; premake picks them up via the glob.

#include <catch2/catch_session.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Astra/Core/TypeContext.hpp>

int main(int argc, char* argv[]) {
    // Install the shared context in the TEST module BEFORE any test computes a
    // component TypeID, so engine/plugin/test agree (TypeID caches per-module).
    Astra::SetTypeContext(&Arcane::Test::SharedTypeContext());
    // Same for Arcane.dll's own module slot, and BEFORE any test runs: a
    // throwaway Runtime installs it and the slot persists after the Runtime
    // dies. This must happen up front because per-type IDs are cached in
    // per-module magic statics and never re-resolve -- pinning later cannot
    // repair an id the DLL already cached.
    // Scoped so it really is throwaway: otherwise it would hold an enkiTS
    // worker pool, an Assets facade and a loaded EngineConfig alive for the
    // whole session.
    {
        Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());
    }
    return Catch::Session().run(argc, argv);
}
