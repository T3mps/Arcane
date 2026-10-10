#pragma once

// ARC_API: dllexport when building Arcane.dll, dllimport for consumers
// (ArcaneRuntime, Arcane Editor, Playground, Game.dll, ArcaneTests). The only C surface
// in the architecture is the plugin entry-point set (M4); everything
// marked ARC_API is direct same-toolchain C++ linkage by design.

#if defined(_WIN32)
    #if defined(ARC_API_EXPORTS)
        #define ARC_API __declspec(dllexport)
    #else
        #define ARC_API __declspec(dllimport)
    #endif
#else
    #define ARC_API __attribute__((visibility("default")))
#endif
