#pragma once

// ARCANE_API: dllexport when building Arcane.dll, dllimport for consumers
// (Loom, Grimoire, Playground, Game.dll, ArcaneTests). The only C surface
// in the architecture is the plugin entry-point set (M4); everything
// marked ARCANE_API is direct same-toolchain C++ linkage by design.

#if defined(_WIN32)
    #if defined(ARCANE_BUILD_DLL)
        #define ARCANE_API __declspec(dllexport)
    #else
        #define ARCANE_API __declspec(dllimport)
    #endif
#else
    #define ARCANE_API __attribute__((visibility("default")))
#endif
