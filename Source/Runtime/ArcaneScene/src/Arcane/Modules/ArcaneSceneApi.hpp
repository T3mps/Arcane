#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_SCENE_STATIC)
    #define ARC_SCENE_API
#elif defined(ARC_SCENE_EXPORTS)
    #define ARC_SCENE_API ARC_DLL_EXPORT
#else
    #define ARC_SCENE_API ARC_DLL_IMPORT
#endif
