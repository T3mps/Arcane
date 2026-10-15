#include "TintOnContact.hpp"

#include <Arcane/Plugin/GameComponents.hpp>

// The one registration line: the ARC_GAME_MODULE prologue (Arcane/Plugin/
// GameModule.hpp) drains every ARC_COMPONENT of the module into its
// ComponentModule (Arcane::Game::RegisterComponents). One .cpp per type.
ARC_COMPONENT(ReferenceProject::TintOnContact)
