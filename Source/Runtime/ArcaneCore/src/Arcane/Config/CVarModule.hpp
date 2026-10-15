#pragma once

// The calling module's own name, for cvar and command registration (settings
// spec s4.3, O1). Every engine, host, tool and test project, and every game
// module, is built with ARC_MODULE_NAME=<its premake project name>
// (premake5.lua's workspace block; build/arcane.lua for game modules and Core
// consumers). Inline, so it expands in the CALLER's module, not in ArcaneCore.dll.

#include <Arcane/Config/CVarRegistry.hpp>

#include <string_view>

#define ARC_MODULE_STRINGIZE2(x) #x
#define ARC_MODULE_STRINGIZE(x) ARC_MODULE_STRINGIZE2(x)
#if defined(ARC_MODULE_NAME)
#define ARC_MODULE_NAME_STRING ARC_MODULE_STRINGIZE(ARC_MODULE_NAME)
#else
#define ARC_MODULE_NAME_STRING "unnamed-module"
#endif

namespace Arcane::Detail
{
    // The innermost CVarModuleScope on this thread (a plugin host names the
    // module it is loading), else this module's ARC_MODULE_NAME.
    [[nodiscard]] inline std::string_view CallerModule() noexcept
    {
        const std::string_view scoped = CVarRegistry::ScopedModule();
        return scoped.empty() ? std::string_view(ARC_MODULE_NAME_STRING) : scoped;
    }
}
