#pragma once

#include <Arcane/Core/ModuleApi.hpp>

namespace Arcane
{
    #if defined(ARC_CLIENT_EXPORTS)
    ARC_DLL_EXPORT
    #else
    ARC_DLL_IMPORT
    #endif
    void InstallImGuiHostHooks() noexcept;
}
