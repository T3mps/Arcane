#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_CLIENT_STATIC)
    #define ARC_CLIENT_API
#elif defined(ARC_CLIENT_EXPORTS) || defined(ARC_CORE_API_EXPORTS)
    #define ARC_CLIENT_API ARC_DLL_EXPORT
#else
    #define ARC_CLIENT_API ARC_DLL_IMPORT
#endif
