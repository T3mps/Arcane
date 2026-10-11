#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_INPUT_STATIC)
    #define ARC_INPUT_API
#elif defined(ARC_INPUT_EXPORTS)
    #define ARC_INPUT_API ARC_DLL_EXPORT
#else
    #define ARC_INPUT_API ARC_DLL_IMPORT
#endif
