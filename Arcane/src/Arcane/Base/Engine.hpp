#pragma once

#include <Arcane/Base/Api.hpp>

namespace Arcane
{
    // Version + build flavor of the loaded engine DLL. Doubles as the
    // simplest possible export for proving the DLL boundary works.
    ARCANE_API const char* BuildInfo();
}
