#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_AUDIO_STATIC)
    #define ARC_AUDIO_API
#elif defined(ARC_AUDIO_EXPORTS)
    #define ARC_AUDIO_API ARC_DLL_EXPORT
#else
    #define ARC_AUDIO_API ARC_DLL_IMPORT
#endif
