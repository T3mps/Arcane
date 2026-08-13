// ArcaneTests runner entry point (Server CommonTests convention).
// Add new test files under Arcane/ArcaneTests/src; premake picks them up via the glob.

#include <catch2/catch_session.hpp>

#include "Helpers/TestTypeContext.hpp"

#include <Arcane/Base/Runtime.hpp>
#include <Astra/Core/TypeContext.hpp>

// Agility SDK handshake (NRI Phase 1, contract §2.3): the D3D12 loader
// reads these EXPORTED symbols from the EXE to redirect device creation
// into the vendored D3D12Core.dll under .\D3D12\. Version must match the
// vendored package; the empirical proof is NRI logging "Using
// ID3D12Device10+" (contract §6 item 3) at the desk milestone.
extern "C" __declspec(dllexport) extern const unsigned D3D12SDKVersion = 619;
extern "C" __declspec(dllexport) extern const char*    D3D12SDKPath    = ".\\D3D12\\";

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
