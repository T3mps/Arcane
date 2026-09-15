#pragma once

// ARCANE_CORE_API: dllexport when building ArcaneCore.dll (ARCANE_CORE_BUILD_DLL is
// defined ONLY in that project's premake block), dllimport for every consumer --
// ArcaneClient.dll, the hosts, arccook/arcbuild, ArcaneTests, ArcaneAssetPipeline's
// objects and every game module. Sibling of ArcaneClient's own export macro
// (Arcane/Base/Api.hpp); a symbol is marked with exactly ONE of the two, by the DLL
// that defines it (spec docs/specs/2026-09-15-core-dll-split-design.md s8, R6).
#if defined(_WIN32)
    #if defined(ARCANE_CORE_BUILD_DLL)
        #define ARCANE_CORE_API __declspec(dllexport)
    #else
        #define ARCANE_CORE_API __declspec(dllimport)
    #endif
#else
    #define ARCANE_CORE_API __attribute__((visibility("default")))
#endif
