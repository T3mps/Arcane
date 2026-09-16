#pragma once

// ARCANE_CORE_API: dllexport when building ArcaneCore.dll (ARCANE_CORE_BUILD_DLL is
// defined ONLY in that project's premake block), dllimport for every consumer --
// ArcaneClient.dll, the hosts, arccook/arcbuild, ArcaneTests, ArcaneAssetPipeline's
// objects and every game module. Sibling of ArcaneClient's own export macro
// (Arcane/Base/Api.hpp); a symbol is marked with exactly ONE of the two, by the DLL
// that defines it (spec docs/specs/2026-09-15-core-dll-split-design.md s8, R6).
// ARCANE_CORE_STATIC comes FIRST and decorates nothing: the branch for a
// consumer that compiles Core FROM SOURCE into a static lib (neither exporting
// nor importing -- dllimport on a declaration whose definition the same build
// compiles is MSVC C2491). No in-tree consumer takes it today: the Gacha Server
// did until Core-DLL split Plan 2 retired its from-source project (2026-09-16),
// and since Plan 3 it links ArcaneCore.dll through build/arcane.lua's
// arcane_core_consumer() like every other consumer. Kept because the branch is
// the documented shape for a future static build (a platform without shared
// libraries), and it costs one #if.
#if defined(ARCANE_CORE_STATIC)
    #define ARCANE_CORE_API
#elif defined(_WIN32)
    #if defined(ARCANE_CORE_BUILD_DLL)
        #define ARCANE_CORE_API __declspec(dllexport)
    #else
        #define ARCANE_CORE_API __declspec(dllimport)
    #endif
#else
    #define ARCANE_CORE_API __attribute__((visibility("default")))
#endif
