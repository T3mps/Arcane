#include "PlayerController2DSystem.hpp"

#include <Arcane/Plugin/GameSystems.hpp>

// The module prologue discovers this default-constructible system. It runs in
// fixed simulation before transform propagation; both roles are intentional
// for this reference input/movement probe.
ARCANE_SYSTEM(
    ReferenceProject::PlayerController2DSystem,
    Arcane::RoleMask::Both,
    Arcane::SystemPhase::FixedUpdate)
