#include "SmokeSystem.hpp"

#include <Arcane/Plugin/GameSystems.hpp>

// ARCANE_GAME_MODULE discovers this declaration while its DLL-owner bracket
// is open. Phase and role are explicit; scheduler order belongs in traits.
ARCANE_SYSTEM(
    TemplateSmoke::SmokeSystem,
    Arcane::RoleMask::Both,
    Arcane::SystemPhase::FixedUpdate)
