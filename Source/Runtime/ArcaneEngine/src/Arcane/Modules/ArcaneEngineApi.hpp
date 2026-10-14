#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_ENGINE_STATIC)
    #define ARC_ENGINE_API
#elif defined(ARC_ENGINE_EXPORTS)
    #define ARC_ENGINE_API ARC_DLL_EXPORT
#else
    #define ARC_ENGINE_API ARC_DLL_IMPORT
#endif
