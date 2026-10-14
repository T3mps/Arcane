#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_ASSET_PIPELINE_STATIC)
    #define ARC_ASSET_PIPELINE_API
#elif defined(ARC_ASSET_PIPELINE_EXPORTS)
    #define ARC_ASSET_PIPELINE_API ARC_DLL_EXPORT
#else
    #define ARC_ASSET_PIPELINE_API ARC_DLL_IMPORT
#endif
