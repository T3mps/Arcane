#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_RENDER_STATIC)
    #define ARC_RENDER_API
#elif defined(ARC_RENDER_EXPORTS)
    #define ARC_RENDER_API ARC_DLL_EXPORT
#else
    #define ARC_RENDER_API ARC_DLL_IMPORT
#endif
