#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_ARC_STATIC)
    #define ARC_ARC_API
#elif defined(ARC_ARC_EXPORTS)
    #define ARC_ARC_API ARC_DLL_EXPORT
#else
    #define ARC_ARC_API ARC_DLL_IMPORT
#endif
