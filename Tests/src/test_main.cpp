// ArcaneTests runner entry point (Server CommonTests convention).
// Add new test files under Arcane/Tests/src; premake picks them up via the glob.

#include <catch2/catch_session.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Astra/Core/TypeContext.hpp>

int main(int argc, char* argv[]) {
    // Install the shared context in the TEST module BEFORE any test computes a
    // component TypeID, so engine/plugin/test agree (TypeID caches per-module).
    Astra::SetTypeContext(&Arcane::Test::SharedTypeContext());
    return Catch::Session().run(argc, argv);
}
