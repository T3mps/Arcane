#pragma once

// editor.layout.* (settings arc S4, spec s7.4). One-off cvars: the name of the
// first is a C++ keyword, so these are not a settings struct.

#include <Arcane/Config/CVarDecl.hpp>

#include <string>

namespace Arcane::Editor
{
    ARC_CVAR_EXTERN(cvar_layoutDefault, std::string);              // editor.layout.default
    ARC_CVAR_EXTERN(cvar_layoutOpenPanelsAtStart, std::string);    // editor.layout.openPanelsAtStart
}
