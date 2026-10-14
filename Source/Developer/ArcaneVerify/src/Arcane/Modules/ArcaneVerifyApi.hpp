#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_VERIFY_STATIC)
    #define ARC_VERIFY_API
#elif defined(ARC_VERIFY_EXPORTS)
    #define ARC_VERIFY_API ARC_DLL_EXPORT
#else
    #define ARC_VERIFY_API ARC_DLL_IMPORT
#endif
