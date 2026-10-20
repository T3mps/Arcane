// 3D-readiness Plan B Task 2 fixture.
//
// Default entity bits and a matching epoch, but GamePlugin_ABIDigest returns a
// stale constant (1), which is not the current mix. The exports are
// hand-written: ARC_GAME_MODULE would return AbiManifestMix().
//
// A real game-module DLL loaded ONLY by the AbiDigestChild process (never by
// ArcaneTests.exe): GamePlugin_Init writes a sentinel file named by the
// ARC_ABI_DIGEST_SENTINEL environment variable, so the test can prove the host
// refused the module BEFORE Init.

#include <Arcane/Core/AbiVersion.hpp>
#include <Arcane/Plugin/GameModule.hpp>

#include <cstdint>
#include <cstdlib>
#include <fstream>

extern "C"
{
    ARC_GAME_MODULE_EXPORT std::uint32_t GamePlugin_ABIVersion() { return ::Arcane::kGamePluginABIVersion; }
    ARC_GAME_MODULE_EXPORT std::uint64_t GamePlugin_ABIDigest() { return 1ull; }
    ARC_GAME_MODULE_EXPORT bool GamePlugin_Init(::Arcane::EngineContext*)
    {
        if (const char* path = std::getenv("ARC_ABI_DIGEST_SENTINEL"))
            std::ofstream(path) << "stale init ran\n";
        return true;
    }
    ARC_GAME_MODULE_EXPORT void GamePlugin_Shutdown() {}
    ARC_GAME_MODULE_EXPORT void GamePlugin_FixedUpdate(double) {}
    ARC_GAME_MODULE_EXPORT void GamePlugin_Update(double, double) {}
    ARC_GAME_MODULE_EXPORT void GamePlugin_SaveState(::Arcane::BinaryWriter&) {}
    ARC_GAME_MODULE_EXPORT bool GamePlugin_LoadState(::Arcane::BinaryReader&) { return true; }
}
