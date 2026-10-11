#pragma once
#include <Arcane/Core/ModuleApi.hpp>

#if defined(ARC_MONOLITHIC) || defined(ARC_EDIT_STATIC)
    #define ARC_EDIT_API
#elif defined(ARC_EDIT_EXPORTS)
    #define ARC_EDIT_API ARC_DLL_EXPORT
#else
    #define ARC_EDIT_API ARC_DLL_IMPORT
#endif
