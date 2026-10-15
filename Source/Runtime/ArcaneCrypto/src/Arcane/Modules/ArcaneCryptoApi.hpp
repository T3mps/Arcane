#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_CRYPTO_STATIC)
    #define ARC_CRYPTO_API
#elif defined(ARC_CRYPTO_EXPORTS)
    #define ARC_CRYPTO_API ARC_DLL_EXPORT
#else
    #define ARC_CRYPTO_API ARC_DLL_IMPORT
#endif
