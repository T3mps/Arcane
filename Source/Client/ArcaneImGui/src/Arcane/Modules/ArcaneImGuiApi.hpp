#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_IMGUI_STATIC)
    #define ARC_IMGUI_API
#elif defined(ARC_IMGUI_EXPORTS)
    #define ARC_IMGUI_API ARC_DLL_EXPORT
#else
    #define ARC_IMGUI_API ARC_DLL_IMPORT
#endif
