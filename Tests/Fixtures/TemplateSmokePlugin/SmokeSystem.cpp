#include "SmokeSystem.hpp"

#include <Arcane/Plugin/GameSystems.hpp>

// ARC_GAME_MODULE discovers this declaration while its DLL-owner bracket
// is open. Phase and role are explicit; scheduler order belongs in traits.
ARC_SYSTEM(
    TemplateSmoke::SmokeSystem,
    Arcane::RoleMask::Both,
    Arcane::SystemPhase::FixedUpdate)
