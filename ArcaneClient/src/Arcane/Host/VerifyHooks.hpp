#pragma once

#include <Arcane/Client/Api.hpp>

namespace Arcane
{
    // Called by developer hosts before parsing verification flags.
    ARC_CLIENT_API void InstallVerifyHooks() noexcept;
}
