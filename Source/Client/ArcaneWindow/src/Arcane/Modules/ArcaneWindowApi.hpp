#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_WINDOW_STATIC)
    #define ARC_WINDOW_API
#elif defined(ARC_WINDOW_EXPORTS)
    #define ARC_WINDOW_API ARC_DLL_EXPORT
#else
    #define ARC_WINDOW_API ARC_DLL_IMPORT
#endif
