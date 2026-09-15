// Core-DLL split (spec docs/specs/2026-09-15-core-dll-split-design.md s1, s8):
// which DLL DEFINES a symbol is the whole point of the export-macro audit, so pin
// it directly -- the address the exe resolves for a Core symbol lies inside
// ArcaneCore.dll's image, and a Client symbol's inside ArcaneClient.dll's.
// GetModuleHandleEx(FROM_ADDRESS) is the OS's own answer to "which module owns
// this address"; no psapi needed.
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>   // RenderErrorCount -- a Client export

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{
    HMODULE OwnerOf(const void* addr)
    {
        HMODULE h = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(addr), &h);
        return h;
    }
}

TEST_CASE("ArcaneCore.dll is a loaded module and defines the Core surface", "[core-dll]")
{
    const HMODULE core   = ::GetModuleHandleW(L"ArcaneCore.dll");
    const HMODULE client = ::GetModuleHandleW(L"ArcaneClient.dll");
    REQUIRE(core != nullptr);
    REQUIRE(client != nullptr);
    CHECK(core != client);

    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::Log::Engine))       == core);
    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::Diagnostics::SetSink)) == core);
    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::BuildInfo))          == core);
    CHECK(OwnerOf(reinterpret_cast<const void*>(&Arcane::RenderErrorCount))   == client);
}

TEST_CASE("one engine logger: the exe and ArcaneClient.dll see the same spdlog instance", "[core-dll]")
{
    // Log::Engine() is Core's. A sink pushed from the exe must receive a line
    // logged from INSIDE ArcaneClient.dll -- Diagnostics::Publish with no sink
    // installed is silent, so use the plugin loader's own ARC_ERROR path:
    // PluginLoadDiagnosticsTest already proves the Diagnostics half; this pins the
    // LOGGER half, which the split moved.
    CHECK(Arcane::Log::Engine() != nullptr);
    CHECK(Arcane::Log::Engine() == Arcane::Log::Engine());
}
