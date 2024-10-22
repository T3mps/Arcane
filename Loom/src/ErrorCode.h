#pragma once

#include "Core/Error.h"
#include "Core/Result.h"

enum class LoomError
{
   LibraryLoadFailure = ARC::ErrorCodeBeginMap::Loom,
   LibraryUnloadFailure,
   InvalidLibraryAccess,

   ModuleLoadFailure,
   ModuleUnloadFailure,

   IncorrectParameterUsage
};
