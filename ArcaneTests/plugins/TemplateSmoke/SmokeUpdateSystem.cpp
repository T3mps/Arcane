#include "SmokeUpdateSystem.hpp"

#include <Arcane/Plugin/GameSystems.hpp>

// ARC_GAME_MODULE discovers this declaration while its DLL-owner bracket
// is open. Phase and role are explicit; scheduler order belongs in traits.
ARC_SYSTEM(
    TemplateSmoke::SmokeUpdateSystem,
    Arcane::RoleMask::Both,
    Arcane::SystemPhase::Update)
