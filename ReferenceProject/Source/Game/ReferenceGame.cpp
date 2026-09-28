#include <Arcane/Plugin/GameModule.hpp>

namespace ReferenceGame
{
    struct Module final : Arcane::GameModule {};
}

ARCANE_GAME_MODULE(ReferenceGame::Module)
