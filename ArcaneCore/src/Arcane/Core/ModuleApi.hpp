#pragma once

#if defined(_WIN32)
    #define ARC_DLL_EXPORT __declspec(dllexport)
    #define ARC_DLL_IMPORT __declspec(dllimport)
#else
    #define ARC_DLL_EXPORT __attribute__((visibility("default")))
    #define ARC_DLL_IMPORT
#endif

#if defined(ARC_MONOLITHIC)
    #define ARC_DLL_EXPORT_IF_MODULAR
#else
    #define ARC_DLL_EXPORT_IF_MODULAR ARC_DLL_EXPORT
#endif

#define ARC_MODULE(Name) extern "C" ARC_DLL_EXPORT_IF_MODULAR void ArcModule_##Name() {}
