#pragma once

#if defined(_WIN32)
    #define ARC_DLL_EXPORT __declspec(dllexport)
    #define ARC_DLL_IMPORT __declspec(dllimport)
#else
    #define ARC_DLL_EXPORT __attribute__((visibility("default")))
    #define ARC_DLL_IMPORT __attribute__((visibility("default")))
#endif

#if defined(ARC_MONOLITHIC)
    #define ARC_DLL_EXPORT_IF_MODULAR
#else
    #define ARC_DLL_EXPORT_IF_MODULAR ARC_DLL_EXPORT
#endif

// One per module (the generate-time declaration check counts them). Besides the
// forced-reference anchor it emits ARC_MODULE_ATTACH (Arcane/Core/ModuleAttach.hpp,
// which the declaring TU includes -- kept out of this header so it stays Astra-free):
// a registrar that installs the process TypeContext in THIS image's Astra slot when
// a host calls Arcane::AttachAllModules (R13).
#define ARC_MODULE(Name) extern "C" ARC_DLL_EXPORT_IF_MODULAR void ArcModule_##Name() {} ARC_MODULE_ATTACH(Name)
