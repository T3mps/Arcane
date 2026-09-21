// Fixture: the input-only game module for arcbuild's multibackend structural
// tests (Task 4, arcbuild multibackend hardening plan). It exists only so
// premake has a real game module to generate build files FOR (gmake/ninja/
// xcode4) -- nothing here is ever compiled by the [build] structural tests
// themselves. See ReferenceProject/Source/Game/ReferenceGame.cpp for the
// pattern this mirrors: every hook has a default, so this module overrides
// none.

#include <Arcane/Plugin/GameModule.hpp>

namespace Fixture
{
    struct Module final : Arcane::GameModule {};
}

ARCANE_GAME_MODULE(Fixture::Module)
