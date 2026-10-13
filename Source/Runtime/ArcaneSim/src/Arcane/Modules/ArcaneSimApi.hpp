#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_SIM_STATIC)
    #define ARC_SIM_API
#elif defined(ARC_SIM_EXPORTS)
    #define ARC_SIM_API ARC_DLL_EXPORT
#else
    #define ARC_SIM_API ARC_DLL_IMPORT
#endif
