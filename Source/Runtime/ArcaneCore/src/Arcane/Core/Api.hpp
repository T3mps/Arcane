#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_CORE_STATIC)
    #define ARC_CORE_API
#elif defined(ARC_CORE_EXPORTS)
    #define ARC_CORE_API ARC_DLL_EXPORT
#else
    #define ARC_CORE_API ARC_DLL_IMPORT
#endif
