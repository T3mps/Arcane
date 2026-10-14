#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_NET_STATIC)
    #define ARC_NET_API
#elif defined(ARC_NET_EXPORTS)
    #define ARC_NET_API ARC_DLL_EXPORT
#else
    #define ARC_NET_API ARC_DLL_IMPORT
#endif
