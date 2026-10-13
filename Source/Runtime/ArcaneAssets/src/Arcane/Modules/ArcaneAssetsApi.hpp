#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_ASSETS_STATIC)
    #define ARC_ASSETS_API
#elif defined(ARC_ASSETS_EXPORTS)
    #define ARC_ASSETS_API ARC_DLL_EXPORT
#else
    #define ARC_ASSETS_API ARC_DLL_IMPORT
#endif
