#include "PlayerController2DSystem.hpp"

#include <Arcane/Plugin/GameSystems.hpp>

// The module prologue discovers this default-constructible system. It runs in
// fixed simulation before transform propagation. It consumes local keyboard
// input, so it does not run on a dedicated server's separate world.
ARCANE_SYSTEM(
    ReferenceProject::PlayerController2DSystem,
    Arcane::RoleMask::Client,
    Arcane::SystemPhase::FixedUpdate)
