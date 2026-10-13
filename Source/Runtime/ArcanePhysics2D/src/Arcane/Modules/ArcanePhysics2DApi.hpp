#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_PHYSICS2D_STATIC)
    #define ARC_PHYSICS2D_API
#elif defined(ARC_PHYSICS2D_EXPORTS)
    #define ARC_PHYSICS2D_API ARC_DLL_EXPORT
#else
    #define ARC_PHYSICS2D_API ARC_DLL_IMPORT
#endif
