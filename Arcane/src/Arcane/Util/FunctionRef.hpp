#pragma once

// Forwarding header: re-exports Arcane::FunctionRef from Core into the Arcane
// module's include root (Arcane/src) so plugin projects (PlaygroundGame,
// HotReloadPlugin*) that only have Arcane/src on their include path can reach
// the type through headers like RunLoop.hpp without needing Core/src on their
// include search path.
//
// The relative include reaches the Core source directly, avoiding a circular
// self-include when Arcane/src is listed before Core/src in the include paths.

#include "../../../../Core/src/Arcane/Util/FunctionRef.hpp"
