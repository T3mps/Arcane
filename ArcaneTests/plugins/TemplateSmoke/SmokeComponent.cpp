#include "SmokeComponent.hpp"

#include <Arcane/Plugin/GameComponents.hpp>

// The one registration line: the ARCANE_GAME_MODULE prologue (Arcane/Plugin/
// GameModule.hpp) drains every ARCANE_COMPONENT of the module into its
// ComponentModule (Arcane::Game::RegisterComponents). One .cpp per type.
ARCANE_COMPONENT(TemplateSmoke::SmokeComponent)
